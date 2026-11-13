#include "syncbox.h"

#ifdef USE_BK72XX

#include <Arduino.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#if defined(SYNCBOX_SATELLITES) || defined(SYNCBOX_CONSOLE)
#include "esphome/components/network/util.h"
#endif

extern "C" {
#include "rtos_pub.h"
#include "arm_arch.h"
#ifdef SYNCBOX_VIDEO
#include "spi_pub.h"
// Prototypy jawnie: w nagłówku SDK są pod #if CFG_USE_SPI_DMA_SLAVE.
int bk_spi_slave_dma_init(UINT32 rate, UINT32 mode);
int bk_spi_slave_dma_xfer(struct spi_message *msg);
#endif
}

namespace esphome::syncbox {

#ifdef ESPHOME_LOG_TAG
ESPHOME_LOG_TAG(TAG, "syncbox");
#else
static const char *const TAG = "syncbox";
#endif

// Kodowanie bitów taśmy w UART2 6N1 @ 6,5 Mbaud (1 bajt UART = 1 bit taśmy), jak w fabrycznym firmware.
// Na płytce jest inwerter między P0 a taśmą.
static const uint8_t UART_BIT1 = 0x20;  // 6x LOW (923 ns) + 2x HIGH (308 ns)  -> po inwersji "1"
static const uint8_t UART_BIT0 = 0x3E;  // 2x LOW (308 ns) + 6x HIGH (923 ns)  -> po inwersji "0"
static const uint32_t UART_BAUD = 6500000;
static const uint8_t LUM_DARK = 30;
// Rejestr statusu FIFO UART2 (BK7231N/BK7238): UART2_BASE_ADDR 0x0802200 + 4*2, bit 17 = TX FIFO pusty
static const uint32_t UART2_FIFO_STATUS_REG = 0x00802208;
static const uint32_t UART_TX_FIFO_EMPTY_BIT = (1u << 17);
// Czarne pasy. Pas to wiersz / kolumna, w ktorej KAZDA komorka jest czarna (najjasniejsza skladowa ponizej LUM_BAR);
// wczesniej liczyla sie srednia jasnosc ponizej 30, wiec ciemna scena albo ciemny pulpit zwijaly kadr do jasnego
// fragmentu na srodku i cala tasma pokazywala jego kolor.
static const uint8_t LUM_BAR = 12;
static const uint8_t CROP_GROW_FRAMES = 3;     // kadr sie powieksza (w pasie pojawil sie obraz): prawie od razu
static const uint8_t CROP_SHRINK_FRAMES = 90;  // kadr sie zmniejsza (nowy pas): dopiero po 1,5 s stalego ukladu
// Doswietlenie ze srodka (w jednostkach tasmy 0..255, po poziomach / balansie / gammie):
static const float MIX_KNEE = 16.0f;         // srodek ciemniejszy niz to nie doswietla wcale
static const float MIX_TAU_US = 200000.0f;   // narastanie doswietlenia (stala czasowa); opadanie natychmiast
static const int MIX_MIN_CELLS = 4;          // najmniejsza glebokosc pasa "srodka"

#ifdef SYNCBOX_VIDEO
static void syncbox_spi_task(void *arg) { static_cast<SyncBox *>(arg)->spi_task_(); }
#endif

// ------------------------------------------------------------------ SyncBox

void SyncBox::setup() {
  if (this->enable_pin_ != nullptr)
    this->enable_pin_->setup();
  // Fabryczny firmware ustawia P24 na 0 przy starcie; pin jest skonfigurowany jako odwrócony,
  // więc "true" = fizycznie LOW = taśma włączona.
  this->set_enable(true);

  Serial2.begin(UART_BAUD, SERIAL_6N1);
  this->uart_ready_ = true;
  this->last_tx_end_us_ = micros();

  this->build_sequence_();
  this->smooth_.assign(this->seq_.size() * 3, 0.0f);
  this->interp_from_.assign(this->seq_.size() * 3, 0.0f);
  this->interp_to_.assign(this->seq_.size() * 3, 0.0f);
  this->interp_cur_.assign(this->seq_.size() * 3, 0.0f);
  this->seg_colors_.assign(this->seq_.size(), Color::BLACK);
  this->mix_v_.assign(this->seq_.size(), 0.0f);
  this->eff_w_ = this->grid_w_;
  this->eff_h_ = this->grid_h_;
  this->crop_ = Crop{0, 0, (uint8_t) (this->grid_w_ - 1), (uint8_t) (this->grid_h_ - 1)};
  this->pending_crop_ = this->crop_;

  this->rebuild_lut_();

#ifdef SYNCBOX_SATELLITES
  // CID nadawcy sACN: z adresu MAC, stale dla urzadzenia
  uint8_t mac[6];
  get_mac_address_raw(mac);
  for (int i = 0; i < 16; i++)
    this->e131_cid_[i] = (uint8_t) (mac[i % 6] ^ (0x5A + i * 17));
#endif

#ifdef SYNCBOX_VIDEO
  if (this->video_enabled_) {
    this->rx_buf_ = new uint8_t[this->frame_size_];     // NOLINT
    this->dn_buf_ = new uint8_t[DN_SEND];               // NOLINT
    memset(this->dn_buf_, 0, DN_SEND);
    this->con_up_.size = 8192;
    this->con_up_.b = new uint8_t[this->con_up_.size];  // NOLINT
    this->con_dn_.size = 4096;
    this->con_dn_.b = new uint8_t[this->con_dn_.size];  // NOLINT
    this->frame_buf_ = new uint8_t[this->frame_size_];  // NOLINT
    memset(this->rx_buf_, 0, this->frame_size_);
    memset(this->frame_buf_, 0, this->frame_size_);

    UINT32 mode = 0;
    if (this->spi_mode_ & 0x01)
      mode |= SPI_CPHA;
    if (this->spi_mode_ & 0x02)
      mode |= SPI_CPOL;
    int r = bk_spi_slave_dma_init(1000000, mode);
    if (r != 0) {
      ESP_LOGE(TAG, "SPI slave DMA init nieudany (%d)", r);
      this->status_set_warning();
    } else {
      OSStatus st = rtos_create_thread((beken_thread_t *) &this->spi_thread_, 6, "syncbox_spi", syncbox_spi_task,
                                       4096, this);
      this->spi_started_ = (st == kNoErr);
      if (!this->spi_started_) {
        ESP_LOGE(TAG, "Nie udało się utworzyć wątku SPI (%d)", (int) st);
        this->status_set_warning();
      }
    }
  }
#endif
}

void SyncBox::dump_config() {
  ESP_LOGCONFIG(TAG,
                "HDMI Sync Box:\n"
                "  Boki L/T/R/B: %u/%u/%u/%u (razem %u), róg startowy %u, %s\n"
                "  Kolejność kolorów taśmy: %u%u%u, reset %u us\n"
                "  Wideo: %s, SPI mode %u, klatka %u B, siatka %ux%u, głębokość %u\n"
                "  Wygładzanie %.2f, nasycenie %.2f, jasność wideo %.2f, letterbox %s, auto-off %s, interpolacja %s",
                this->sides_[0], this->sides_[1], this->sides_[2], this->sides_[3], this->total_segments(),
                this->start_corner_, this->clockwise_ ? "zgodnie z zegarem" : "przeciwnie do zegara",
                this->order_[0], this->order_[1], this->order_[2], (unsigned) this->reset_us_,
                YESNO(this->video_enabled_), this->spi_mode_, this->frame_size_, this->grid_w_, this->grid_h_,
                this->depth_, this->smoothing_, this->saturation_, this->video_brightness_, YESNO(this->letterbox_),
                YESNO(this->auto_off_), YESNO(this->interpolate_));
  LOG_PIN("  Enable pin: ", this->enable_pin_);
  ESP_LOGCONFIG(TAG,
                "  Odświeżanie taśmy co %u ms\n"
                "  Kalibracja obrazu: poziomy %u..%u, balans R/G/B %.2f/%.2f/%.2f, gamma %.2f\n"
                "  Doświetlenie ze środka: %.0f %%",
                (unsigned) this->refresh_interval_ms_, this->level_black_, this->level_white_, this->wb_[0],
                this->wb_[1], this->wb_[2], this->video_gamma_, this->inner_mix_ * 100.0f);
#ifdef SYNCBOX_VIDEO
  ESP_LOGCONFIG(TAG, "  Wątek SPI: %s", this->spi_started_ ? "działa" : "NIE działa");
#endif
#ifdef SYNCBOX_SATELLITES
  ESP_LOGCONFIG(TAG, "  Satelity (E1.31): %u, włączone: %s", (unsigned) this->satellites_.size(),
                YESNO(this->satellites_enabled_));
  for (auto &sat : this->satellites_) {
    static const char *const MODES[] = {"średni kolor", "kopia taśmy", "lewy bok",   "górny bok",
                                        "prawy bok",    "dolny bok",   "całe pole",  "obwód pokoju"};
    ESP_LOGCONFIG(TAG, "    %s: %s, universe %u, %s, %u px, jasność %.2f, max %u fps, przejście %u ms, gamma %.1f",
                  sat.name.empty() ? "(bez nazwy)" : sat.name.c_str(),
                  sat.address.empty() ? "multicast" : sat.address.c_str(), sat.universe,
                  MODES[sat.mode < 8 ? sat.mode : 0], sat.leds, sat.brightness,
                  (unsigned) (1000000 / (sat.min_interval_us ? sat.min_interval_us : 1)),
                  (unsigned) (sat.tau_us / 1000), sat.gamma);
  }
#endif
}

void SyncBox::set_enable(bool on) {
  if (this->enable_pin_ != nullptr)
    this->enable_pin_->digital_write(on);
  this->enabled_ = on;
}

void SyncBox::set_mode(SyncBoxMode m) {
  if (m == this->mode_)
    return;
  this->mode_ = m;
  ESP_LOGI(TAG, "Tryb: %s", m == MODE_VIDEO ? "wideo (HDMI)" : "światło");
  if (m == MODE_LIGHT) {
    if (this->light_ != nullptr)
      this->light_->restore_static();
  } else {
    std::fill(this->smooth_.begin(), this->smooth_.end(), 0.0f);
    std::fill(this->mix_v_.begin(), this->mix_v_.end(), 0.0f);
    if (!this->signal_ && this->auto_off_)
      this->push_black_();
  }
}

void SyncBox::build_sequence_() {
  // Pierścień zgodnie z zegarem, start w lewym dolnym rogu:
  // lewy (dół->góra), górny (lewo->prawo), prawy (góra->dół), dolny (prawo->lewo).
  std::vector<Segment> ring;
  for (uint8_t side = 0; side < 4; side++)
    for (uint8_t i = 0; i < this->sides_[side]; i++)
      ring.push_back(Segment{side, i});
  const int n = ring.size();
  this->seq_.clear();
  if (n == 0)
    return;
  int start = 0;
  switch (this->start_corner_) {
    case 1:  // top_left = początek boku górnego
      start = this->sides_[0];
      break;
    case 2:  // top_right = początek boku prawego
      start = this->sides_[0] + this->sides_[1];
      break;
    case 3:  // bottom_right = początek boku dolnego
      start = this->sides_[0] + this->sides_[1] + this->sides_[2];
      break;
    default:
      start = 0;
  }
  for (int k = 0; k < n; k++) {
    int idx = this->clockwise_ ? (start + k) % n : ((start - 1 - k) % n + n) % n;
    this->seq_.push_back(ring[idx]);
  }
#ifdef SYNCBOX_SATELLITES
  for (uint8_t side = 0; side < 4; side++)
    this->side_pos_[side].assign(this->sides_[side], 0);
  for (size_t i = 0; i < this->seq_.size(); i++)
    this->side_pos_[this->seq_[i].side][this->seq_[i].index] = (uint8_t) i;
#endif
}

void SyncBox::transmit(const uint8_t *rgb, size_t num_leds, bool light_on) {
  if (!this->uart_ready_ || num_leds == 0)
    return;
  const size_t need = num_leds * 24;
  if (this->tx_buf_.size() < need)
    this->tx_buf_.resize(need);

  size_t k = 0;
  for (size_t i = 0; i < num_leds; i++) {
    const uint8_t *p = rgb + i * 3;
    for (uint8_t c = 0; c < 3; c++) {
      const uint8_t v = p[this->order_[c]];
      for (int8_t bit = 7; bit >= 0; bit--)
        this->tx_buf_[k++] = (v & (1u << bit)) ? UART_BIT1 : UART_BIT0;
    }
  }

  // zasilanie tasmy podaza za stanem swiatla, nie za kolorami (ciemna scena != wylaczone)
  if (light_on && !this->enabled_) {
    this->set_enable(true);
    delayMicroseconds(2000);
  }

  // odstęp (reset) od końca poprzedniej ramki
  const uint32_t since = micros() - this->last_tx_end_us_;
  if (since < this->reset_us_)
    delayMicroseconds(this->reset_us_ - since);

  {
    InterruptLock lock;  // ~0,8 ms dla 28 sekcji; bez dziur w strumieniu
    Serial2.write(this->tx_buf_.data(), k);
    while ((REG_READ(UART2_FIFO_STATUS_REG) & UART_TX_FIFO_EMPTY_BIT) == 0) {
    }
  }
  delayMicroseconds(3);  // ostatni bajt w rejestrze przesuwnym
  this->last_tx_end_us_ = micros();
  this->last_tx_ms_ = millis();
  this->frames_out_++;

  this->update_average_(rgb, num_leds, light_on);
#ifdef SYNCBOX_SATELLITES
  if (!light_on) {
    this->sat_pending_ = false;
    this->send_satellites_(rgb, num_leds, false, true);  // czern od razu
  } else if (!this->satellites_.empty() && !this->sat_hold_) {
    // Wysylka UDP trwa ok. 2 ms na lampe; zrobiona tutaj opoznialaby ustawienie czasomierza zapisu posredniego
    // (dithering) i ten zapis wypadal. Bufor jest odkladany, a pakiety ida z loop(), juz po ustawieniu czasomierza.
    // Po zapisie posrednim (sat_hold_) nie wysylamy: wtedy trwa juz odbior nastepnej ramki po SPI i wysylka w tym
    // czasie psula ok. 1 ramke na sekunde.
    this->sat_rgb_.assign(rgb, rgb + num_leds * 3);
    this->sat_pending_ = true;
  }
#endif

  if (!light_on && this->enabled_ && this->enable_follows_light_)
    this->set_enable(false);  // czern juz wyslana, mozna odciac zasilanie
}

void SyncBox::push_black_() {
  std::fill(this->smooth_.begin(), this->smooth_.end(), 0.0f);
  std::fill(this->mix_v_.begin(), this->mix_v_.end(), 0.0f);
  std::fill(this->interp_from_.begin(), this->interp_from_.end(), 0.0f);
  std::fill(this->interp_to_.begin(), this->interp_to_.end(), 0.0f);
  this->interp_done_ = true;
  std::fill(this->seg_colors_.begin(), this->seg_colors_.end(), Color::BLACK);
  if (this->light_ != nullptr)
    this->light_->push_video(this->seg_colors_.data(), this->seg_colors_.size());
  this->blanked_ = true;
}

void SyncBox::loop() {
  const uint32_t now = millis();
#ifdef SYNCBOX_VIDEO
  this->link_loop_();
  if (this->frame_ready_) {
    this->process_frame_();
#ifdef SYNCBOX_SATELLITES
    // Satelita `full` wysyla cale pole pozniej, z konca loop(). Kopia teraz, poki bufor nalezy do petli: po zwolnieniu
    // flagi watek SPI wpisuje do frame_buf_ nastepna klatke i pakiety szlyby z dwoch klatek naraz.
    if (this->sat_full_) {
      const uint8_t *f = this->frame_buf_ + this->pix_off_;
      this->full_snap_.assign(f, f + (size_t) this->eff_w_ * this->eff_h_ * 3);
    }
#endif
    this->frame_ready_ = false;
  }
  if (this->interpolate_ && !this->interp_done_)
    this->interpolate_tick_(micros());
  // Sygnal "jest" dopiero po 500 ms ciaglych ramek: uklad wideo bez wejscia HDMI potrafi wystawiac
  // pojedyncze ramki ze smieciowa synchronizacja i czujnik migalby on/off.
  // "Sa ramki" = licznik ramek zmienil sie w ciagu signal_timeout. Czas liczony od chwili, w ktorej petla to zauwazyla
  // (wlasny `now`), wiec roznica nigdy nie jest ujemna, a po zaniku ramek stan wraca dopiero z nowa ramka.
  const uint32_t ft = this->frames_total_;
  if (ft != this->sig_seen_frames_) {
    this->sig_seen_frames_ = ft;
    this->sig_seen_ms_ = now;
    this->sig_stale_ = false;
  } else if (!this->sig_stale_ && (uint32_t) (now - this->sig_seen_ms_) >= this->signal_timeout_ms_) {
    this->sig_stale_ = true;
  }
  // ESP32 z firmware v14.4+ podaje stan wejscia wprost; bez sygnalu konwerter potrafi wystawiac przypadkowa
  // synchronizacje i czujnik skakal. Starszy firmware (brak bloku pomocniczego) - jak dotad, po samych ramkach.
  const bool input_ok = !this->esp_fresh_ || (this->esp_flags_ & 0x03) == 0x03;
  const bool recent = (ft > 0) && !this->sig_stale_ && input_ok;
  if (!recent)
    this->signal_start_ms_ = 0;
  else if (this->signal_start_ms_ == 0)
    this->signal_start_ms_ = now ? now : 1;
  const bool sig = recent && (uint32_t) (now - this->signal_start_ms_) >= 500;
  if (sig != this->signal_) {
    this->signal_ = sig;
    ESP_LOGI(TAG, "Sygnał z układu wideo: %s", sig ? "jest" : "brak");
#ifdef USE_BINARY_SENSOR
    if (this->signal_sensor_ != nullptr)
      this->signal_sensor_->publish_state(sig);
#endif
    if (!sig && this->mode_ == MODE_VIDEO && this->auto_off_)
      this->push_black_();
  }
#endif
  // Tryb wideo bez sygnalu (np. tuz po starcie, zanim uklad wideo ruszy): nie zostawiaj
  // na tasmie odtworzonego koloru statycznego (zwykle biel), tylko czern.
  if (this->auto_off_ && !this->signal_ && !this->blanked_ && this->video_active_())
    this->push_black_();

  // Okresowe odswiezanie: jesli przez refresh_interval nic nie poszlo do tasmy, wyslij bufor ponownie
  // (uklady SM16703 po zaklóceniu albo po wlaczeniu zasilania moga trzymac smieci / biel).
  if (this->refresh_interval_ms_ != 0 && this->light_ != nullptr && this->light_->is_on() &&
      (int32_t) (now - this->last_tx_ms_) >= (int32_t) this->refresh_interval_ms_)
    this->light_->refresh();  // (roznica bez znaku dawala tu drugi zapis tasmy zaraz po kazdej klatce)

  if ((uint32_t) (now - this->last_stats_ms_) >= 1000) {
    const float dt = (now - this->last_stats_ms_) / 1000.0f;
    const uint32_t fin = this->frames_total_;
    this->video_fps_ = (fin - this->stats_frames_in_) / dt;
    this->stats_frames_in_ = fin;
    this->led_fps_ = (this->frames_out_ - this->stats_frames_out_) / dt;
    this->stats_frames_out_ = this->frames_out_;
    this->last_stats_ms_ = now;
#ifdef USE_SENSOR
    if (this->video_fps_sensor_ != nullptr)
      this->video_fps_sensor_->publish_state(this->video_fps_);
    if (this->led_fps_sensor_ != nullptr)
      this->led_fps_sensor_->publish_state(this->led_fps_);
#endif
  }
#ifdef USE_TEXT_SENSOR
  if (this->avg_sensor_ != nullptr && this->avg_dirty_ && (uint32_t) (now - this->avg_published_ms_) >= 500) {
    const Color c = this->avg_color_;
    const int dr = abs((int) c.r - this->avg_published_.r), dg = abs((int) c.g - this->avg_published_.g),
              db = abs((int) c.b - this->avg_published_.b);
    if (dr + dg + db >= 6 || !this->avg_sensor_->has_state()) {
      char buf[8];
      snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
      this->avg_sensor_->publish_state(buf);
      this->avg_published_ = c;
      this->avg_published_ms_ = now;
    }
    this->avg_dirty_ = false;
  }
#endif
#ifdef SYNCBOX_SATELLITES
  if (this->sat_pending_) {
    this->sat_pending_ = false;
    this->send_satellites_(this->sat_rgb_.data(), this->sat_rgb_.size() / 3, true, false);
  }
#endif
}

bool SyncBox::video_active_() const {
  return this->mode_ == MODE_VIDEO && this->light_ != nullptr && this->light_->is_on();
}

// Tablica: wartosc z klatki (0..255) -> po poziomach, balansie bieli i gammie. Przeliczana po zmianie parametrow.
void SyncBox::rebuild_lut_() {
  const float span = (float) (this->level_white_ - this->level_black_);
  for (int c = 0; c < 3; c++) {
    for (int i = 0; i < 256; i++) {
      float v = ((float) i - this->level_black_) / (span > 0 ? span : 1.0f);
      v = clamp(v, 0.0f, 1.0f) * this->wb_[c];
      v = clamp(v, 0.0f, 1.0f);
      if (this->video_gamma_ != 1.0f)
        v = powf(v, this->video_gamma_);
      this->lut_[c][i] = (uint8_t) (v * 255.0f + 0.5f);
      this->lutf_[c][i] = v * 255.0f;
    }
  }
  this->lut_dirty_ = false;
}

void SyncBox::update_average_(const uint8_t *rgb, size_t num_leds, bool light_on) {
  Color c = Color::BLACK;
  if (light_on && num_leds > 0) {
    uint32_t r = 0, g = 0, b = 0;
    for (size_t i = 0; i < num_leds; i++) {
      r += rgb[i * 3];
      g += rgb[i * 3 + 1];
      b += rgb[i * 3 + 2];
    }
    c = Color((uint8_t) (r / num_leds), (uint8_t) (g / num_leds), (uint8_t) (b / num_leds));
  }
  if (c.r != this->avg_color_.r || c.g != this->avg_color_.g || c.b != this->avg_color_.b) {
    this->avg_color_ = c;
    this->avg_dirty_ = true;
  }
}

void SyncBox::set_satellites_enabled(bool b) {
  if (b == this->satellites_enabled_)
    return;
  this->satellites_enabled_ = b;
  ESP_LOGI(TAG, "Satelity: %s", b ? "włączone" : "wyłączone");
#ifdef SYNCBOX_SATELLITES
  if (!b) {
    // zgas satelity: ramka czerni, bez limitu fps
    std::vector<uint8_t> black(this->seq_.size() * 3, 0);
    this->satellites_enabled_ = true;
    this->send_satellites_(black.data(), this->seq_.size(), false, true);
    this->satellites_enabled_ = false;
  } else if (this->light_ != nullptr) {
    this->light_->refresh();
  }
#endif
}

#ifdef SYNCBOX_SATELLITES
void SyncBox::add_satellite(const std::string &name, const std::string &address, uint16_t universe, uint8_t mode,
                            uint16_t leds, float brightness, uint8_t max_fps, uint32_t transition_ms, float gamma) {
  Satellite sat;
  sat.name = name;
  sat.address = address;
  sat.universe = universe;
  sat.mode = mode;
  if (mode == SAT_FULL)
    this->sat_full_ = true;
  sat.leds = leds == 0 ? 1 : (leds > 170 ? 170 : leds);
  sat.brightness = clamp(brightness, 0.0f, 1.0f);
  sat.min_interval_us = 1000000 / (max_fps == 0 ? 1 : max_fps);
  sat.tau_us = transition_ms * 1000u;
  sat.gamma = gamma < 1.0f ? 1.0f : gamma;
  memset(&sat.addr, 0, sizeof(sat.addr));
  this->satellites_.push_back(sat);
}

void SyncBox::set_satellite_blur(uint8_t radius, bool closed) {
  if (this->satellites_.empty())
    return;
  this->satellites_.back().blur = radius;
  this->satellites_.back().closed = closed;
}

void SyncBox::add_satellite_segment(float u0, float u1, uint16_t leds) {
  if (this->satellites_.empty() || leds == 0)
    return;
  this->satellites_.back().segs.push_back(SatSegment{u0, u1, leds});
}

// Punkt u obwodu ekranu (opis u przy SatSegment) jako mieszanka dwoch sekcji tasmy TV. Miedzy srodkami sasiednich
// sekcji tego samego boku interpolacja liniowa; bok bez sekcji (zwykle dolny) to plynne przejscie od ostatniej sekcji
// boku przed nim do pierwszej sekcji boku za nim. false = tasma bez sekcji.
bool SyncBox::ring_tap_(float u, SatTap &tap) const {
  u -= 4.0f * floorf(u / 4.0f);
  int side = (int) u;
  if (side > 3)
    side = 3;
  const float t = u - (float) side;
  float f;  // udzial drugiej sekcji
  const int n = (int) this->side_pos_[side].size();
  if (n > 0) {
    float x = t * (float) n - 0.5f;
    if (x < 0.0f)
      x = 0.0f;
    if (x > (float) (n - 1))
      x = (float) (n - 1);
    const int i0 = (int) x;
    const int i1 = i0 + 1 < n ? i0 + 1 : n - 1;
    f = x - (float) i0;
    tap.pa = this->side_pos_[side][i0];
    tap.pb = this->side_pos_[side][i1];
  } else {
    int before = 0, after = 0;  // ile pustych bokow przed tym i za tym (w ciagu pustych)
    while (before < 3 && this->side_pos_[(side + 3 - before) & 3].empty())
      before++;
    while (after < 3 && this->side_pos_[(side + 1 + after) & 3].empty())
      after++;
    const auto &prev = this->side_pos_[(side + 3 - before) & 3];
    const auto &next = this->side_pos_[(side + 1 + after) & 3];
    if (prev.empty() || next.empty())
      return false;
    f = ((float) before + t) / (float) (before + 1 + after);
    tap.pa = prev.back();
    tap.pb = next.front();
  }
  tap.fw = (uint16_t) (f * 256.0f + 0.5f);
  return true;
}

// Raz, przy pierwszej wysylce: wagi diod (SAT_ROOM), bufor stanu i tablica kodowania gamma.
void SyncBox::prepare_satellite_(Satellite &sat) {
  sat.prepared = true;
  size_t px = 1;  // tryby jednego koloru
  if (sat.mode == SAT_ROOM) {
    sat.taps.clear();
    for (const auto &sg : sat.segs) {
      for (uint16_t j = 0; j < sg.leds && sat.taps.size() < 170; j++) {
        SatTap tap{0, 0, 0};
        this->ring_tap_(sg.u0 + (sg.u1 - sg.u0) * ((float) j + 0.5f) / (float) sg.leds, tap);
        sat.taps.push_back(tap);
      }
    }
    px = sat.taps.size();
  } else if (sat.mode == SAT_MIRROR) {
    px = std::min<size_t>(this->seq_.size(), 170);
  }
  sat.state.assign(px * 3, 0);
  sat.enc.clear();
  if (sat.gamma > 1.001f) {
    sat.enc.resize(1021);
    const float inv = 1.0f / sat.gamma;
    for (int i = 0; i <= 1020; i++)
      sat.enc[i] = (uint8_t) (255.0f * powf((float) i / 1020.0f, inv) + 0.5f);
  }
}

// Jeden pakiet E1.31 (sACN, ANSI E1.31-2018): root + framing + DMP, start code 0, `channels` bajtow danych.
bool SyncBox::send_e131_(Satellite &sat, const uint8_t *data, size_t channels, uint16_t universe_offset) {
  const uint16_t universe = sat.universe + universe_offset;
  if (sat.failed)
    return false;
  if (!this->udp_) {
    this->udp_ = socket::socket_ip(SOCK_DGRAM, IPPROTO_IP);
    if (!this->udp_) {
      ESP_LOGW(TAG, "Satelity: nie można otworzyć gniazda UDP");
      return false;
    }
    this->udp_->setblocking(false);
  }
  if (!sat.resolved) {
    std::string ip = sat.address;
    if (ip.empty()) {
      char b[24];
      snprintf(b, sizeof(b), "239.255.%u.%u", (unsigned) (sat.universe >> 8), (unsigned) (sat.universe & 0xFF));
      ip = b;
    }
    sat.addrlen = socket::set_sockaddr((struct sockaddr *) &sat.addr, sizeof(sat.addr), ip, 5568);
    if (sat.addrlen == 0) {
      ESP_LOGE(TAG, "Satelita %s: zły adres '%s'", sat.name.c_str(), ip.c_str());
      sat.failed = true;
      return false;
    }
    sat.resolved = true;
  }
  if (channels > 510)
    channels = 510;
  uint8_t pkt[126 + 511];
  const uint16_t dmx = (uint16_t) (1 + channels);
  const uint16_t total = (uint16_t) (125 + dmx);  // 125 B naglowkow + kod startowy + kanaly (bylo o 1 za duzo)
  memset(pkt, 0, 126);
  // root layer
  pkt[0] = 0x00;
  pkt[1] = 0x10;  // preamble size
  memcpy(pkt + 4, "ASC-E1.17\0\0\0", 12);
  uint16_t fl = (uint16_t) (0x7000 | (total - 16));
  pkt[16] = fl >> 8;
  pkt[17] = fl & 0xFF;
  pkt[21] = 0x04;  // VECTOR_ROOT_E131_DATA
  memcpy(pkt + 22, this->e131_cid_, 16);
  // framing layer
  fl = (uint16_t) (0x7000 | (total - 38));
  pkt[38] = fl >> 8;
  pkt[39] = fl & 0xFF;
  pkt[43] = 0x02;  // VECTOR_E131_DATA_PACKET
  strncpy((char *) pkt + 44, "ESPHome HDMI Sync Box", 63);
  pkt[108] = 100;  // priorytet
  pkt[111] = sat.seq++;
  pkt[113] = universe >> 8;
  pkt[114] = universe & 0xFF;
  // DMP layer
  fl = (uint16_t) (0x7000 | (total - 115));
  pkt[115] = fl >> 8;
  pkt[116] = fl & 0xFF;
  pkt[117] = 0x02;  // VECTOR_DMP_SET_PROPERTY
  pkt[118] = 0xA1;  // address & data type
  pkt[122] = 0x01;  // address increment
  pkt[123] = dmx >> 8;
  pkt[124] = dmx & 0xFF;
  pkt[125] = 0x00;  // DMX start code
  memcpy(pkt + 126, data, channels);
  // Multicast (bez `address`): kazdy universe ma wlasna grupe 239.255.<hi>.<lo>. Zapamietany adres dotyczy pierwszego
  // universe lampy; kolejne (tryb `full`) dostaja adres wyliczony z numeru - wczesniej szly do grupy pierwszego.
  struct sockaddr_storage dst_buf;
  const struct sockaddr *dst = (const struct sockaddr *) &sat.addr;
  socklen_t dst_len = sat.addrlen;
  if (universe_offset != 0 && sat.address.empty()) {
    char b[24];
    snprintf(b, sizeof(b), "239.255.%u.%u", (unsigned) (universe >> 8), (unsigned) (universe & 0xFF));
    const socklen_t l = socket::set_sockaddr((struct sockaddr *) &dst_buf, sizeof(dst_buf), b, 5568);
    if (l != 0) {
      dst = (const struct sockaddr *) &dst_buf;
      dst_len = l;
    }
  }
  const ssize_t w = this->udp_->sendto(pkt, total, 0, dst, dst_len);
  if (w < 0) {
    this->sat_errors_++;
    return false;
  }
  this->sat_packets_++;
  return true;
}

void SyncBox::send_satellites_(const uint8_t *rgb, size_t num_leds, bool light_on, bool force) {
  if (this->satellites_.empty() || !this->satellites_enabled_ || !network::is_connected())
    return;
  const uint32_t now = micros();
  // Nalezny jest satelita, ktoremu minal jego odstep (z tolerancja 3 ms, zeby przy max_fps rownym czestosci klatek
  // szedl w kazdej klatce, a przy polowie - rowno co druga).
  const uint32_t tol = 3000;
  bool any_due = force;
  for (auto &sat : this->satellites_)
    if ((uint32_t) (now - sat.last_tx_us) + tol >= sat.min_interval_us)
      any_due = true;
  if (!any_due)
    return;

  // srednie: cala tasma + kazdy bok (wg kolejnosci sekcji seq_)
  uint32_t sum[5][3] = {{0}};
  uint32_t cnt[5] = {0};
  const size_t n = std::min(num_leds, this->seq_.size());
  for (size_t i = 0; i < n; i++) {
    const uint8_t side = this->seq_[i].side < 4 ? this->seq_[i].side : 0;
    for (uint8_t c = 0; c < 3; c++) {
      sum[4][c] += rgb[i * 3 + c];
      sum[side][c] += rgb[i * 3 + c];
    }
    cnt[4]++;
    cnt[side]++;
  }

  uint8_t data[510];
  for (auto &sat : this->satellites_) {
    if (!force && (uint32_t) (now - sat.last_tx_us) + tol < sat.min_interval_us)
      continue;
    const uint32_t t_sat = micros();
    size_t channels = 0;
    const float br = light_on ? sat.brightness : 0.0f;
    if (sat.mode == SAT_FULL) {
#ifdef SYNCBOX_VIDEO
      // cala siatka po LUT (poziomy, balans bieli, gamma), wierszami od gory, 170 px na universe
      // z kopii pola (loop()): do frame_buf_ watek SPI moze juz wpisywac nastepna klatke
      const size_t total_px = this->full_snap_.size() / 3;
      const uint8_t *f = this->full_snap_.data();
      const uint8_t ir = this->pix_order_[0], ig = this->pix_order_[1], ib = this->pix_order_[2];
      if (this->lut_dirty_)
        this->rebuild_lut_();
      for (size_t start = 0, u = 0; start < total_px; start += 170, u++) {
        const size_t m = std::min<size_t>(170, total_px - start);
        for (size_t i = 0; i < m; i++) {
          const uint8_t *p = f + (start + i) * 3;
          data[i * 3] = (uint8_t) (this->lut_[0][p[ir]] * br + 0.5f);
          data[i * 3 + 1] = (uint8_t) (this->lut_[1][p[ig]] * br + 0.5f);
          data[i * 3 + 2] = (uint8_t) (this->lut_[2][p[ib]] * br + 0.5f);
        }
        this->send_e131_(sat, data, m * 3, (uint16_t) u);
      }
#endif
      sat.last_tx_us = now;
      const uint32_t us_full = micros() - t_sat;      // czas obslugi liczony takze dla `full`
      this->sat_us_sum_ += us_full;
      this->sat_us_cnt_++;
      if (us_full > this->sat_us_max_)
        this->sat_us_max_ = us_full;
      continue;
    }
    // Pozostale tryby: kolor docelowy kazdej diody (1/256 poziomu tasmy) -> wygladzanie w czasie -> jasnosc ->
    // kodowanie gamma pod odbiornik. Wejscie to bufor tasmy po ditheringu (skacze o 1 poziom miedzy zapisami), wiec
    // bez wygladzania i z gamma odbiornika ciemne kolory migotalyby.
    if (!sat.prepared)
      this->prepare_satellite_(sat);
    const size_t px = sat.state.size() / 3;
    static int32_t tgt[170 * 3];
    if (sat.mode == SAT_ROOM) {
      for (size_t k = 0; k < px; k++) {
        const SatTap &tp = sat.taps[k];
        if (tp.pa >= num_leds || tp.pb >= num_leds) {
          tgt[k * 3] = tgt[k * 3 + 1] = tgt[k * 3 + 2] = 0;
          continue;
        }
        for (uint8_t c = 0; c < 3; c++)
          tgt[k * 3 + c] = (int32_t) rgb[tp.pa * 3 + c] * (256 - tp.fw) + (int32_t) rgb[tp.pb * 3 + c] * tp.fw;
      }
      // Rozmycie wzdluz tasmy lampy: dwa razy srednia z diod w promieniu `blur` (razem okno trojkatne +-2 x blur).
      // W petli zamknietej okno przechodzi przez poczatek tasmy, w otwartej jest przycinane do koncow.
      if (sat.blur != 0 && px > 1) {
        static int32_t tmp[170 * 3];
        const int r = sat.blur, n = (int) px;
        for (uint8_t pass = 0; pass < 2; pass++) {
          const int32_t *src = pass == 0 ? tgt : tmp;
          int32_t *dst = pass == 0 ? tmp : tgt;
          for (int k = 0; k < n; k++) {
            int32_t acc[3] = {0, 0, 0};
            int m = 0;
            for (int j = k - r; j <= k + r; j++) {
              int q = j;
              if (sat.closed) {
                q %= n;
                if (q < 0)
                  q += n;
              } else if (q < 0 || q >= n) {
                continue;
              }
              acc[0] += src[q * 3];
              acc[1] += src[q * 3 + 1];
              acc[2] += src[q * 3 + 2];
              m++;
            }
            dst[k * 3] = acc[0] / m;
            dst[k * 3 + 1] = acc[1] / m;
            dst[k * 3 + 2] = acc[2] / m;
          }
        }
      }
    } else if (sat.mode == SAT_MIRROR) {
      for (size_t i = 0; i < px * 3; i++)
        tgt[i] = i < num_leds * 3 ? (int32_t) rgb[i] << 8 : 0;
    } else {
      int idx = 4;  // average
      if (sat.mode >= SAT_LEFT && sat.mode <= SAT_BOTTOM)
        idx = sat.mode - SAT_LEFT;
      if (cnt[idx] == 0)
        idx = 4;
      for (uint8_t c = 0; c < 3; c++)
        tgt[c] = cnt[idx] != 0 ? (int32_t) ((sum[idx][c] << 8) / cnt[idx]) : 0;
    }
    // udzial nowej wartosci: 1 - exp(-dt / tau); przy zgaszonym swietle i bez wygladzania od razu
    int32_t a16 = 65536;
    if (light_on && sat.tau_us != 0) {
      uint32_t dt = now - sat.last_tx_us;
      if (dt < sat.tau_us * 8u)
        a16 = (int32_t) (65536.0f * (1.0f - expf(-(float) dt / (float) sat.tau_us)));
    }
    const int32_t br8 = (int32_t) (br * 256.0f + 0.5f);
    const bool use_enc = !sat.enc.empty();
    const size_t out_px = (sat.mode == SAT_ROOM || sat.mode == SAT_MIRROR) ? px : sat.leds;
    for (size_t k = 0; k < out_px; k++) {
      const size_t sp = px == 1 ? 0 : k;
      for (uint8_t c = 0; c < 3; c++) {
        if (k < px) {
          int32_t &st = sat.state[sp * 3 + c];
          st += (int32_t) (((int64_t) (tgt[sp * 3 + c] - st) * a16) >> 16);
        }
        int32_t v = (sat.state[sp * 3 + c] * br8) >> 8;  // 1/256 poziomu
        v = v < 0 ? 0 : (v > 65280 ? 65280 : v);
        data[k * 3 + c] = use_enc ? sat.enc[(v + 32) >> 6] : (uint8_t) ((v + 128) >> 8);
      }
    }
    channels = out_px * 3;
    // Diagnostyka (przelacznik "log sekcji"), raz na sekunde dla satelity "obwod pokoju": kolory sekcji tasmy TV
    // (sat-we: L, G, P, D jak w logu sekcji), kolory docelowe diod lampy przed wygladzaniem (sat-cel) i bajty wyslane do
    // lampy (sat-wy), do 75 pierwszych diod.
    if (sat.mode == SAT_ROOM && this->log_sections_ && (uint32_t) (millis() - this->sat_log_ms_) >= 1000) {
      this->sat_log_ms_ = millis();
      static char buf[470];
      int p = 0;
      static const char NAMES[4] = {'L', 'G', 'P', 'D'};
      buf[0] = 0;
      for (uint8_t side = 0; side < 4; side++) {
        if (this->side_pos_[side].empty())
          continue;
        p += snprintf(buf + p, sizeof(buf) - p, "%c:", NAMES[side]);
        for (uint8_t pos : this->side_pos_[side])
          if (pos < num_leds && p < (int) sizeof(buf) - 8)
            p += snprintf(buf + p, sizeof(buf) - p, "%02X%02X%02X ", rgb[pos * 3], rgb[pos * 3 + 1], rgb[pos * 3 + 2]);
      }
      ESP_LOGI(TAG, "sat-we %s", buf);
      p = 0;
      buf[0] = 0;
      for (size_t i = 0; i < px && i < 75; i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%02X%02X%02X", (unsigned) ((tgt[i * 3] + 128) >> 8),
                      (unsigned) ((tgt[i * 3 + 1] + 128) >> 8), (unsigned) ((tgt[i * 3 + 2] + 128) >> 8));
      ESP_LOGI(TAG, "sat-cel %s", buf);
      p = 0;
      buf[0] = 0;
      for (size_t i = 0; i < out_px && i < 75; i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%02X%02X%02X", data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
      ESP_LOGI(TAG, "sat-wy %s", buf);
    }
    this->send_e131_(sat, data, channels);
    sat.last_tx_us = now;
    const uint32_t us = micros() - t_sat;
    this->sat_us_sum_ += us;
    this->sat_us_cnt_++;
    if (us > this->sat_us_max_)
      this->sat_us_max_ = us;
  }
}
#endif

#ifdef SYNCBOX_VIDEO
// CRC16 CCITT (0x1021, start 0xFFFF), po pol bajtu z tablicy 16 pozycji.
static uint16_t crc16_(const uint8_t *d, size_t n) {
  static const uint16_t T[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF};
  uint16_t c = 0xFFFF;
  while (n--) {
    const uint8_t v = *d++;
    c = (uint16_t) ((c << 4) ^ T[((c >> 12) ^ (v >> 4)) & 0x0F]);
    c = (uint16_t) ((c << 4) ^ T[((c >> 12) ^ v) & 0x0F]);
  }
  return c;
}

// Blok pomocniczy z ESP32 (za polem obrazu). Wolane z watku SPI.
bool SyncBox::parse_aux_(const uint8_t *a) {
  if (a[0] != 'S' || a[1] != 'B' || a[2] != 'X' || a[3] != '1')
    return false;
  if (crc16_(a, AUX_LEN - 2) != (uint16_t) ((a[AUX_LEN - 2] << 8) | a[AUX_LEN - 1])) {
    this->aux_bad_ = this->aux_bad_ + 1;
    return false;
  }
  this->aux_gen_ = this->aux_gen_ + 1;
  memcpy(this->aux_raw_, a, sizeof(this->aux_raw_));
  this->aux_seen_ms_ = millis();
  this->aux_gen_ = this->aux_gen_ + 1;
  this->aux_frames_ = this->aux_frames_ + 1;
  if (!this->tun_active_)
    return true;
  if (this->tun_resetting_) {
    if (a[4] & 0x10)  // ESP32 wyzerowal swoja strone tunelu
      this->tun_resetting_ = false;
    return true;
  }
  if (a[4] & 0x20) {  // ESP32 dopiero wystartowal: numeracja porcji od zera po obu stronach
    this->con_reset_ms_ = millis();
    this->tun_resetting_ = true;
    return true;
  }
  if (this->dn_len_ != 0 && a[22] == this->dn_seq_)
    this->dn_len_ = 0;  // nasza porcja doszla
  const uint16_t ul = (uint16_t) (a[24] | (a[25] << 8));
  if (ul != 0 && ul <= AUX_LEN - 28 && a[21] != this->up_last_ && this->con_up_.space() >= ul) {
    this->con_up_.write(a + 26, ul);
    this->up_last_ = a[21];
  }
  return true;
}

// Blok zwrotny dla ESP32 (linia MISO). Zwraca dlugosc do wyslania; 0 = nic (nie ma klienta konsoli), wtedy SPI pracuje
// tylko na odbior, dokladnie jak przed v17. Wolane z watku SPI przed uzbrojeniem transferu.
uint16_t SyncBox::build_dn_() {
  if (!this->tun_active_ || this->dn_buf_ == nullptr)
    return 0;
  uint8_t *p = this->dn_buf_ + 8;  // 8 bajtow zerowych z przodu: pierwsze bajty nadajnika slave bywaja zgubione
  uint16_t len = 0;
  uint8_t flags = 0x01;
  if (this->tun_resetting_) {
    flags |= 0x02;
    if (!this->reset_applied_) {
      this->con_dn_.drop_all();
      this->dn_len_ = 0;
      this->dn_seq_ = 0;
      this->up_last_ = 0;
      this->reset_applied_ = true;
    }
  } else {
    this->reset_applied_ = false;
    if (this->dn_len_ == 0) {
      const uint32_t n = this->con_dn_.read(this->dn_pay_, DN_MAX);
      if (n != 0) {
        this->dn_len_ = (uint16_t) n;
        this->dn_seq_ = this->dn_seq_ == 255 ? 1 : (uint8_t) (this->dn_seq_ + 1);
      }
    }
    len = this->dn_len_;
  }
  p[0] = 'S';
  p[1] = 'B';
  p[2] = 'D';
  p[3] = '1';
  p[4] = flags;
  p[5] = this->dn_seq_;
  p[6] = this->up_last_;
  p[7] = (uint8_t) len;
  p[8] = (uint8_t) (len >> 8);
  memcpy(p + 9, this->dn_pay_, len);
  const uint16_t crc = crc16_(p, 9 + (size_t) len);
  p[9 + len] = (uint8_t) (crc >> 8);
  p[10 + len] = (uint8_t) crc;
  return DN_SEND;
}

void SyncBox::spi_task_() {
  struct spi_message msg;
  const uint16_t fs = this->frame_size_;
  while (true) {
    // znacznik na końcu bufora: po transferze mówi, czy klatka była pełna
    this->rx_buf_[fs - 1] = 0xA5;
    this->rx_buf_[fs - 2] = 0x5A;
    this->rx_buf_[fs - 3] = 0xA5;
    if (fs >= 19)
      this->rx_buf_[15] = 0;  // koniec naglowka: transfer krotszy niz naglowek nie przejdzie na starych danych
    const uint16_t tx_len = this->build_dn_();
    msg.send_buf = tx_len != 0 ? this->dn_buf_ : nullptr;
    msg.send_len = tx_len;
    msg.recv_buf = this->rx_buf_;
    msg.recv_len = fs;
    bk_spi_slave_dma_xfer(&msg);  // blokuje do odebrania fs bajtów lub do końca CS
    const bool complete =
        !(this->rx_buf_[fs - 1] == 0xA5 && this->rx_buf_[fs - 2] == 0x5A && this->rx_buf_[fs - 3] == 0xA5);
    const uint8_t *h = this->rx_buf_;
    const bool hdr_ok = fs >= 19 && h[0] == 'S' && h[1] == 'B' && h[2] == 'G' && h[3] == '2' && h[6] == (uint8_t) ~h[4] &&
                        h[7] == (uint8_t) ~h[5] && h[12] == 0x5A && h[13] == 0xA5 && h[14] == 0xC3 && h[15] == 0x3C &&
                        (h[11] & 1);
    const uint32_t n = hdr_ok ? (uint32_t) h[4] * h[5] * 3u : 0;
    if (!complete) {
      // Ramka krotsza niz bufor (ESP32 wysyla naglowek + tyle bajtow, ile ma pole): przyjmij, jesli naglowek
      // jest poprawny, pole miesci sie w buforze i zgadza sie suma kontrolna. Pole 0 x 0 = ramka bez obrazu.
      bool ok = false;
      if (hdr_ok && 16u + n <= (uint32_t) fs) {
        if (n == 0) {
          ok = true;
        } else if (n >= 3) {
          uint32_t sum = 0;
          for (uint32_t i = 0; i < n; i++)
            sum += h[16 + i];
          ok = (uint16_t) sum == (uint16_t) (h[9] | (h[10] << 8));
        }
      }
      if (!ok) {
        this->frames_partial_++;
        continue;
      }
    }
    // blok pomocniczy zaraz za polem (firmware ESP32 v14.4+): stan wejscia HDMI i tunel konsoli
    if (hdr_ok && 16u + n + AUX_LEN <= (uint32_t) fs)
      this->parse_aux_(h + 16 + n);
    if (hdr_ok && n == 0)
      continue;  // ramka bez obrazu nie jest klatka wideo
    this->frames_total_++;
    this->last_frame_ms_ = millis();
    if (!this->frame_ready_) {
      memcpy(this->frame_buf_, this->rx_buf_, fs);
      this->frame_ready_ = true;
      this->enable_loop_soon_any_context();
    } else {
      this->frames_dropped_++;  // petla glowna nie zdazyla przetworzyc poprzedniej klatki
    }
  }
}

// Stan z ESP32 -> czujniki i log; obsluga klienta konsoli. Wolane z loop().
void SyncBox::link_loop_() {
  const uint32_t now = millis();
  uint8_t st[24];
  uint32_t g0, g1;
  int tries = 0;
  do {  // kopia spojna: watek SPI zwieksza aux_gen_ przed i po zapisie
    g0 = this->aux_gen_;
    memcpy(st, this->aux_raw_, sizeof(st));
    g1 = this->aux_gen_;
  } while ((g0 != g1 || (g0 & 1)) && ++tries < 4);
  const bool st_ok = tries < 4;  // false: cztery proby z rzedu trafily na zapis watku SPI - `st` moze byc sklejone
  const bool ever = this->aux_frames_ != 0;
  this->esp_fresh_ = ever && (uint32_t) (now - this->aux_seen_ms_) < 1500;
  if (this->esp_fresh_ && st_ok) {
    this->esp_flags_ = st[4];
    if (this->esp_ver_logged_ != st[20]) {
      this->esp_ver_logged_ = st[20];
      ESP_LOGI(TAG, "ESP32: firmware v%u.%u%s", st[20] / 10, st[20] % 10, (st[4] & 0x08) ? " (na próbie)" : "");
    }
    const uint16_t drops = (uint16_t) (st[12] | (st[13] << 8));
    if (!this->esp_dropouts_known_ || drops != this->esp_dropouts_) {
      if (this->esp_dropouts_known_ && drops > this->esp_dropouts_)
        ESP_LOGW(TAG, "HDMI: zanik sygnału na wejściu (od startu ESP32: %u)", drops);
      this->esp_dropouts_ = drops;
      this->esp_dropouts_known_ = true;
#ifdef USE_SENSOR
      if (this->hdmi_dropouts_sensor_ != nullptr)
        this->hdmi_dropouts_sensor_->publish_state(drops);
#endif
    }
  }
  // tekst stanu czyta `st` tylko przy swiezych danych; z niespojnej kopii nie publikujemy (nastepne przejscie petli)
  if (ever && (st_ok || !this->esp_fresh_) && (uint32_t) (now - this->link_pub_ms_) >= 500) {
    this->link_pub_ms_ = now;
    char txt[48];
    if (!this->esp_fresh_) {
      snprintf(txt, sizeof(txt), "brak danych z ESP32");
    } else if (!(st[4] & 0x01)) {
      snprintf(txt, sizeof(txt), "brak sygnału");
    } else {
      static const char *const HDR[] = {"SDR", "HDR10", "HDR10+", "Dolby Vision", "HLG", "HDR?"};
      const uint16_t w = (uint16_t) (st[6] | (st[7] << 8)), hh = (uint16_t) (st[8] | (st[9] << 8));
      snprintf(txt, sizeof(txt), "%ux%u%c%u %s", w, hh, st[11] >= 32 && st[11] < 127 ? (char) st[11] : '?', st[10],
               st[5] < 6 ? HDR[st[5]] : "?");
    }
    if (this->hdmi_published_ != txt) {
      this->hdmi_published_ = txt;
      ESP_LOGI(TAG, "HDMI wejście: %s", txt);
#ifdef USE_TEXT_SENSOR
      if (this->hdmi_input_sensor_ != nullptr)
        this->hdmi_input_sensor_->publish_state(txt);
#endif
    }
  }
  this->console_loop_();
}

#ifdef SYNCBOX_CONSOLE
void SyncBox::console_close_(const char *why) {
  this->tun_active_ = false;
  this->con_client_.reset();
  ESP_LOGI(TAG, "Konsola ESP32: klient rozłączony (%s)", why);
}

// Serwer TCP konsoli ESP32: bajty od klienta ida tunelem do konsoli ESP32, wyjscie konsoli wraca do klienta.
// Jeden klient naraz. Gdy klient jest podlaczony, do kazdej ramki SPI dochodzi blok zwrotny po MISO.
void SyncBox::console_loop_() {
  if (this->console_port_ == 0)
    return;
  if (!this->con_server_) {
    if (this->con_server_failed_ || !network::is_connected())
      return;
    this->con_server_ = socket::socket_ip(SOCK_STREAM, 0);
    bool ok = static_cast<bool>(this->con_server_);
    if (ok) {
      int en = 1;
      this->con_server_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &en, sizeof(en));
      this->con_server_->setblocking(false);
      struct sockaddr_storage sa;
      const socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &sa, sizeof(sa), this->console_port_);
      ok = sl != 0 && this->con_server_->bind((struct sockaddr *) &sa, sl) == 0 && this->con_server_->listen(1) == 0;
    }
    if (!ok) {
      ESP_LOGE(TAG, "Konsola ESP32: nie można otworzyć portu %u", this->console_port_);
      this->con_server_.reset();
      this->con_server_failed_ = true;
      return;
    }
    ESP_LOGI(TAG, "Konsola ESP32: port TCP %u%s", this->console_port_,
             this->console_password_.empty() ? " (BEZ HASŁA)" : "");
  }
  if (!this->con_client_) {
    struct sockaddr_storage src;
    socklen_t al = sizeof(src);
    auto c = this->con_server_->accept((struct sockaddr *) &src, &al);
    if (!c)
      return;
    int en = 1;
    c->setsockopt(IPPROTO_TCP, TCP_NODELAY, &en, sizeof(en));
    c->setblocking(false);
    this->con_client_ = std::move(c);
    this->con_authed_ = this->console_password_.empty();
    this->con_auth_buf_.clear();
    this->con_up_.drop_all();
    this->tun_resetting_ = true;  // obie strony tunelu zaczynaja od zera
    this->tun_active_ = true;
    this->con_reset_ms_ = millis();
    ESP_LOGI(TAG, "Konsola ESP32: klient podłączony");
    return;
  }
  if (this->tun_resetting_) {
    // czekamy, az ESP32 potwierdzi zerowanie tunelu (2-3 ramki); bez odpowiedzi (stary firmware ESP32, brak linii
    // MISO) klient dostaje komunikat i jest rozlaczany, zeby nie blokowal portu
    if ((uint32_t) (millis() - this->con_reset_ms_) > 3000) {
      static const char MSG[] = "ERR ESP32 nie odpowiada na laczu zwrotnym\n";
      this->con_client_->write(MSG, sizeof(MSG) - 1);
      this->tun_resetting_ = false;
      this->console_close_("ESP32 nie odpowiada");
    }
    return;
  }
  static uint8_t buf[1024];
  // klient -> ESP32
  uint32_t want = this->con_authed_ ? this->con_dn_.space() : 64;
  if (want > sizeof(buf))
    want = sizeof(buf);
  if (want != 0) {
    const ssize_t n = this->con_client_->read(buf, want);
    if (n == 0) {
      this->console_close_("koniec połączenia");
      return;
    }
    if (n < 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) {
        this->console_close_("błąd odczytu");
        return;
      }
    } else if (this->con_authed_) {
      this->con_dn_.write(buf, (uint32_t) n);
    } else {
      // pierwsza linia od klienta to haslo
      for (ssize_t i = 0; i < n; i++) {
        if (this->con_authed_) {
          this->con_dn_.write(buf + i, 1);
        } else if (buf[i] == '\n') {
          if (this->con_auth_buf_ == this->console_password_) {
            this->con_authed_ = true;
          } else {
            this->con_client_->write("ERR haslo\n", 10);
            this->console_close_("złe hasło");
            return;
          }
        } else if (buf[i] != '\r' && this->con_auth_buf_.size() < 80) {
          this->con_auth_buf_.push_back((char) buf[i]);
        }
      }
    }
  }
  // ESP32 -> klient
  if (this->con_authed_) {
    uint32_t n = this->con_up_.avail();
    if (n > sizeof(buf))
      n = sizeof(buf);
    if (n != 0) {
      for (uint32_t i = 0; i < n; i++)
        buf[i] = this->con_up_.b[(this->con_up_.r + i) % this->con_up_.size];
      const ssize_t w = this->con_client_->write(buf, n);
      if (w > 0) {
        this->con_up_.r = this->con_up_.r + (uint32_t) w;
      } else if (w < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        this->console_close_("błąd zapisu");
        return;
      }
    }
  }
}
#else
void SyncBox::console_loop_() {}
void SyncBox::console_close_(const char *) {}
#endif

void SyncBox::update_crop_(const uint8_t *rows, const uint8_t *cols, const uint8_t *cols_mid) {
  // rows / cols: najjasniejsza skladowa w wierszu / kolumnie pola; cols_mid: to samo z wierszy wokol srodka ekranu
  const int W = this->eff_w_, H = this->eff_h_;
  // Najwiekszy pas z jednej strony: 1/4 wysokosci i 1/4 szerokosci (obraz 2,39:1 to 12,5 % wysokosci, 4:3 to 12,5 %
  // szerokosci, 1:1 to 22 %). Szerszy czarny obszar (pionowe wideo, logo na czarnym tle) nie jest pasem: boki
  // zostaja ciemne albo dostaja doswietlenie ze srodka.
  const int max_v = H / 4, max_h = W / 4;
  bool any_bright = false;
  for (int y = 0; y < H; y++)
    if (rows[y] >= LUM_DARK)
      any_bright = true;
  if (!any_bright)
    return;  // czarny ekran: nie zmieniaj kadru
  int top = 0, bot = 0, left = 0, right = 0;
  // Ramki z naglowkiem pochodza z firmware ESP32, ktory sam wycina pasy gorny i dolny z dokladnoscia do linii:
  // drugie wykrywanie tutaj mogloby tylko uciac ciemna gore albo dol samego obrazu.
  if (this->pix_off_ == 0) {
    while (top <= max_v && top < H && rows[top] < LUM_BAR)
      top++;
    while (bot <= max_v && bot < H && rows[H - 1 - bot] < LUM_BAR)
      bot++;
    if (top > max_v)
      top = 0;  // ciemny obszar wiekszy niz jakikolwiek pas: to obraz, nie pas
    if (bot > max_v)
      bot = 0;
  }
  while (left <= max_h && left < W && cols[left] < LUM_BAR)
    left++;
  while (right <= max_h && right < W && cols[W - 1 - right] < LUM_BAR)
    right++;
  // Rozmycie w ESP32 (wzdluz gornej i dolnej krawedzi poziome) wciaga obraz do 2-3 kolumn pasa, wiec pas liczony
  // z calych kolumn jest za waski. W srodkowych wierszach rozmycie jest pionowe: tam granica jest ostra, z jedna
  // kolumna polcienia. Przesuwamy granice do pierwszej kolumny samego obrazu.
  const auto refine = [&](int n, bool from_left) {
    if (n == 0 || n > max_h)
      return n;
    const auto at = [&](int i) { return (int) cols_mid[from_left ? i : W - 1 - i]; };
    for (int k = 0; k < 3 && n < max_h && at(n) < LUM_BAR; k++)
      n++;
    if (n < max_h && n + 1 < W && at(n) * 2 < at(n + 1))
      n++;  // kolumna polcienia
    return n;
  };
  left = refine(left, true);
  right = refine(right, false);
  if (left > max_h || right > max_h) {
    left = right = 0;
  } else {
    // pasy boczne sa symetryczne; rozne szerokosci oznaczaja ciemny fragment obrazu z jednej strony
    const int d = left > right ? left - right : right - left;
    left = right = d > 1 ? 0 : std::min(left, right);
  }
  const Crop cand{(uint8_t) left, (uint8_t) top, (uint8_t) (W - 1 - right), (uint8_t) (H - 1 - bot)};
  if (cand == this->crop_) {
    this->pending_crop_ = cand;
    this->pending_count_ = 0;
    return;
  }
  if (cand == this->pending_crop_) {
    if (this->pending_count_ < 255)
      this->pending_count_++;
  } else {
    this->pending_crop_ = cand;
    this->pending_count_ = 0;
  }
  const bool grows = cand.x0 <= this->crop_.x0 && cand.x1 >= this->crop_.x1 && cand.y0 <= this->crop_.y0 &&
                     cand.y1 >= this->crop_.y1;
  if (this->pending_count_ >= (grows ? CROP_GROW_FRAMES : CROP_SHRINK_FRAMES)) {
    this->crop_ = cand;
    ESP_LOGI(TAG, "Kadr: x %u..%u, y %u..%u", cand.x0, cand.x1, cand.y0, cand.y1);
  }
}

void SyncBox::cell_range_(uint8_t side, uint8_t index, uint8_t &xa, uint8_t &xb, uint8_t &ya, uint8_t &yb) const {
  const int cx0 = this->crop_.x0, cx1 = this->crop_.x1, cy0 = this->crop_.y0, cy1 = this->crop_.y1;
  const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
  int n = this->sides_[side];
  if (n <= 0)
    n = 1;
  const int d = this->depth_;
  const int off = this->side_offset_[side];

  auto span = [](int base, int total, int count, int i, bool reverse, int &a, int &b) {
    const float step = (float) total / (float) count;
    const int k = reverse ? (count - 1 - i) : i;
    a = base + (int) floorf(k * step + 1e-4f);
    b = base + (int) ceilf((k + 1) * step - 1e-4f) - 1;
    if (b < a)
      b = a;
  };

  int a = 0, b = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
  switch (side) {
    case SIDE_LEFT:  // index 0 = dół
      span(cy0, ch, n, index, true, a, b);
      y0 = a;
      y1 = b;
      x0 = cx0 + off;
      x1 = x0 + d - 1;
      break;
    case SIDE_TOP:  // index 0 = lewo
      span(cx0, cw, n, index, false, a, b);
      x0 = a;
      x1 = b;
      y0 = cy0 + off;
      y1 = y0 + d - 1;
      break;
    case SIDE_RIGHT:  // index 0 = góra
      span(cy0, ch, n, index, false, a, b);
      y0 = a;
      y1 = b;
      x1 = cx1 - off;
      x0 = x1 - d + 1;
      break;
    default:  // SIDE_BOTTOM, index 0 = prawo
      span(cx0, cw, n, index, true, a, b);
      x0 = a;
      x1 = b;
      y1 = cy1 - off;
      y0 = y1 - d + 1;
      break;
  }
  const int gw = this->eff_w_ - 1, gh = this->eff_h_ - 1;
  x0 = std::clamp(x0, 0, gw);
  x1 = std::clamp(x1, 0, gw);
  y0 = std::clamp(y0, 0, gh);
  y1 = std::clamp(y1, 0, gh);
  if (x1 < x0)
    x1 = x0;
  if (y1 < y0)
    y1 = y0;
  xa = x0;
  xb = x1;
  ya = y0;
  yb = y1;
}

void SyncBox::process_frame_() {
  // Ramka z firmware ESP32 v10: 16 B naglowka ('S','B','G','2', szerokosc, wysokosc, ich negacje, licznik, suma bajtow pola,
  // 1, 5A A5 C3 3C), potem pole wierszami, reszta zera. Uklad pola moze sie zmieniac z klatki na klatke.
  // Ramka bez naglowka (fabryczny firmware): pole grid_width x grid_height od poczatku ramki.
  {
    const uint8_t *h = this->frame_buf_;
    uint8_t w = this->grid_w_, hh = this->grid_h_;
    uint16_t off = 0;
    if (this->frame_size_ >= 16 && h[0] == 'S' && h[1] == 'B' && h[2] == 'G' && h[3] == '2' && h[6] == (uint8_t) ~h[4] &&
        h[7] == (uint8_t) ~h[5] && h[12] == 0x5A && h[13] == 0xA5 && h[14] == 0xC3 && h[15] == 0x3C && h[4] >= 1 &&
        h[4] <= 64 && h[5] >= 1 && h[5] <= 64 && 16u + (uint32_t) h[4] * h[5] * 3u <= this->frame_size_) {
      w = h[4];
      hh = h[5];
      off = 16;
      this->headerless_run_ = 0;
      if (h[11] & 1) {  // suma bajtow pola: wykrywa przeklamania na laczu SPI
        uint32_t sum = 0;
        const uint8_t *p = h + 16;
        const size_t n = (size_t) w * hh * 3;
        for (size_t i = 0; i < n; i++)
          sum += p[i];
        if ((uint16_t) sum != (uint16_t) (h[9] | (h[10] << 8))) {
          this->frames_bad_++;
          return;
        }
      }
    } else {
      // Naglowek uszkodzony albo pole wieksze niz bufor: nie traktuj tego jak ramki bez naglowka (ominelaby sume kontrolna
      // i zresetowala kadr). Do ramek bez naglowka (fabryczny firmware) wracamy dopiero po 60 takich ramkach z rzedu.
      const bool magic = h[0] == 'S' && h[1] == 'B' && h[2] == 'G' && h[3] == '2';
      if (magic || (this->pix_off_ == 16 && ++this->headerless_run_ < 60)) {
        this->frames_bad_++;
        return;
      }
      if ((uint32_t) w * hh * 3u > this->frame_size_)
        return;  // ramka za krotka na skonfigurowane pole
    }
    if (w != this->eff_w_ || hh != this->eff_h_ || off != this->pix_off_) {
      this->eff_w_ = w;
      this->eff_h_ = hh;
      this->pix_off_ = off;
      this->crop_ = Crop{0, 0, (uint8_t) (w - 1), (uint8_t) (hh - 1)};
      this->pending_crop_ = this->crop_;
      this->pending_count_ = 0;
      std::fill(this->mix_v_.begin(), this->mix_v_.end(), 0.0f);
      ESP_LOGI(TAG, "Pole z ukladu wideo: %u x %u%s", (unsigned) w, (unsigned) hh, off ? " (naglowek)" : "");
    }
  }
  const int W = this->eff_w_;
  const uint8_t *f = this->frame_buf_ + this->pix_off_;
  const int H = this->eff_h_;
  const uint8_t ir = this->pix_order_[0], ig = this->pix_order_[1], ib = this->pix_order_[2];

  if (this->letterbox_) {
    uint8_t rows[64] = {0}, cols[64] = {0}, cols_mid[64] = {0};  // najjasniejsza skladowa w wierszu / kolumnie
    // wiersze wokol srodka ekranu (ok. 1/4 wysokosci): tam rozmycie pola w ESP32 jest pionowe takze 1/4 szerokosci
    // od bocznej krawedzi, wiec granica pasa jest ostra
    const int ym0 = H * 3 / 8, ym1 = H - 1 - H * 3 / 8;
    for (int y = 0; y < H; y++) {
      const uint8_t *p = f + y * W * 3;
      const bool mid = y >= ym0 && y <= ym1;
      uint8_t rm = 0;
      for (int x = 0; x < W; x++, p += 3) {
        const uint8_t l = std::max(p[0], std::max(p[1], p[2]));
        if (l > rm)
          rm = l;
        if (l > cols[x])
          cols[x] = l;
        if (mid && l > cols_mid[x])
          cols_mid[x] = l;
      }
      rows[y] = rm;
    }
    this->update_crop_(rows, cols, cols_mid);
  }

  if (this->lut_dirty_)
    this->rebuild_lut_();
  const float sat = this->saturation_, vb = this->video_brightness_, alpha = 1.0f - this->smoothing_;
  const size_t n = this->seq_.size();
  const float mix = this->inner_mix_;
  const bool mix_on = mix > 0.0f && this->mix_v_.size() >= n;
  const float mix_att = std::min(1.0f, (float) this->frame_interval_us_ / MIX_TAU_US);
  for (size_t s = 0; s < n; s++) {
    uint8_t xa, xb, ya, yb;
    this->cell_range_(this->seq_[s].side, this->seq_[s].index, xa, xb, ya, yb);
    uint32_t r = 0, g = 0, b = 0, cnt = 0;
    for (int y = ya; y <= yb; y++) {
      for (int x = xa; x <= xb; x++) {
        const uint8_t *p = f + (y * W + x) * 3;
        r += p[ir];
        g += p[ig];
        b += p[ib];
        cnt++;
      }
    }
    if (cnt == 0)
      cnt = 1;
    // poziomy czerni/bieli, balans bieli, gamma (tablica bez zaokraglen, interpolowana dla ulamkowej sredniej)
    const auto lut = [this, cnt](int c, uint32_t sum) -> float {
      const float a = (float) sum / (float) cnt;
      int i = (int) a;
      if (i >= 255)
        return this->lutf_[c][255];
      const float t = a - (float) i;
      return this->lutf_[c][i] + (this->lutf_[c][i + 1] - this->lutf_[c][i]) * t;
    };
    float fr = lut(0, r), fg = lut(1, g), fb = lut(2, b);
    // nasycenie
    const float gray = (fr + fg + fb) / 3.0f;
    fr = clamp(gray + (fr - gray) * sat, 0.0f, 255.0f);
    fg = clamp(gray + (fg - gray) * sat, 0.0f, 255.0f);
    fb = clamp(gray + (fb - gray) * sat, 0.0f, 255.0f);
    // Doswietlenie ze srodka. Sekcja pokazuje kolor krawedzi; gdy krawedz jest duzo ciemniejsza od obrazu w glebi
    // (pas tej sekcji od krawedzi do osi kadru), dostaje brakujace swiatlo w kolorze srodka, do `mix` jego jasnosci.
    // Bez progu wlaczenia: dodatek maleje plynnie do zera, gdy krawedz jasnieje, wiec nic nie pulsuje; kolor krawedzi
    // nie jest ani przyciemniany, ani wzmacniany (szum ciemnej krawedzi zostaje maly). Jasnosc = najjasniejsza skladowa.
    // Liczone po nasyceniu obu kolorow, wiec najjasniejsza skladowa wyniku to dokladnie `mix` jasnosci srodka,
    // a slaby kolor krawedzi nie jest wycinany przez nasycenie mieszanki.
    if (mix_on) {
      float &vs = this->mix_v_[s];
      uint8_t ixa = xa, ixb = xb, iya = ya, iyb = yb;
      if (!this->inner_range_(this->seq_[s].side, ixa, ixb, iya, iyb)) {
        vs = 0.0f;
      } else {
        uint32_t mr = 0, mg = 0, mb = 0;
        for (int y = iya; y <= iyb; y++) {
          const uint8_t *p = f + (y * W + ixa) * 3;
          for (int x = ixa; x <= ixb; x++, p += 3) {  // srednia swiatla (po tablicy), nie kodow obrazu
            mr += this->lut_[0][p[ir]];
            mg += this->lut_[1][p[ig]];
            mb += this->lut_[2][p[ib]];
          }
        }
        const float inv = 1.0f / (float) ((iyb - iya + 1) * (ixb - ixa + 1));
        float m_r = (float) mr * inv, m_g = (float) mg * inv, m_b = (float) mb * inv;
        const float mgray = (m_r + m_g + m_b) / 3.0f;  // to samo nasycenie co dla krawedzi
        m_r = clamp(mgray + (m_r - mgray) * sat, 0.0f, 255.0f);
        m_g = clamp(mgray + (m_g - mgray) * sat, 0.0f, 255.0f);
        m_b = clamp(mgray + (m_b - mgray) * sat, 0.0f, 255.0f);
        const float vm = std::max(m_r, std::max(m_g, m_b)), ve = std::max(fr, std::max(fg, fb));
        vs += (vm - vs) * mix_att;                 // narasta ze stala czasowa MIX_TAU_US ...
        const float veff = vm < vs ? vm : vs;      // ... opada od razu
        const float lift = mix * (veff - MIX_KNEE) - ve;
        if (lift > 0.0f) {                         // tu vm > MIX_KNEE, wiec dzielnik nie jest maly
          const float k = lift / vm;
          fr += k * m_r;
          fg += k * m_g;
          fb += k * m_b;
        }
      }
    }
    // jasność trybu wideo
    fr *= vb;
    fg *= vb;
    fb *= vb;
    // wygładzanie w czasie
    float *sm = &this->smooth_[s * 3];
    sm[0] += (fr - sm[0]) * alpha;
    sm[1] += (fg - sm[1]) * alpha;
    sm[2] += (fb - sm[2]) * alpha;
    this->seg_colors_[s] = Color((uint8_t) (sm[0] + 0.5f), (uint8_t) (sm[1] + 0.5f), (uint8_t) (sm[2] + 0.5f));
  }

  if (!this->video_active_()) {
    this->log_stats_();
    return;
  }

  const uint32_t now_us = micros();
  if (this->last_frame_us_ != 0) {
    const uint32_t dt = now_us - this->last_frame_us_;
    if (dt > 2000 && dt < 2000000)
      this->frame_interval_us_ = (this->frame_interval_us_ * 3 + dt) / 4;  // srednia kroczaca odstepu klatek
  }
  this->last_frame_us_ = now_us;

  // Przed uznaniem sygnalu (500 ms ciaglych ramek) nic nie idzie na tasme: wczesniej kazda ramka blyskala na ok. 1 ms
  // i byla zaraz gaszona. Wygladzanie i okres klatek licza sie dalej.
  if (this->auto_off_ && !this->signal_) {
    this->log_stats_();
    return;
  }

  // Interpolacja ma sens tylko przy wolnym zrodle (fabryczny ESP32: 10-12 fps). Przy >= 40 fps kolejna
  // klatka przychodzi, zanim przejscie zdazy zrobic drugi krok (pierwszy, t=0, nic nie zmienia), wiec
  // tasma stalaby w miejscu - wtedy klatki ida na tasme bezposrednio.
  if (!this->interpolate_ || this->frame_interval_us_ < 25000) {
    this->interp_done_ = true;
    const bool two = this->light_->get_dithering() && this->frame_interval_us_ >= 8000;
    this->light_->set_subframes(two);
    this->light_->push_video_f(this->smooth_.data(), this->seg_colors_.data(), n);
    this->blanked_ = false;
    // Dithering: drugi zapis tasmy w polowie okresu klatki (poziomy posrednie co 1/4, wzor najwolniej 30 Hz).
    // Wczesniej drugi zapis szedl zaraz po pierwszym, wiec pierwszy swiecil ok. 1 ms i poziomy posrednie byly przeklamane.
    if (two) {
      // Chwila zapisu: polowa okresu liczona od odebrania klatki (nie od konca jej przetwarzania), zeby oba zapisy
      // dzielily okres rowno i zeby drugi skonczyl sie na dlugo przed odbiorem nastepnej ramki po SPI (ramka 48x27
      // przy 8 MHz zajmuje ostatnie 3,9 ms okresu, a zapis tasmy wylacza przerwania na ok. 1 ms). Spozniony o wiecej
      // niz do 5/8 okresu jest pomijany.
      const uint32_t half_us = this->frame_interval_us_ / 2;
      const uint32_t elapsed = micros() - this->last_frame_us_;
      const uint32_t delay_ms = elapsed + 1000 < half_us ? (half_us - elapsed + 500) / 1000 : 1;
      this->set_timeout("dith2", delay_ms, [this]() {
        const uint32_t since = micros() - this->last_frame_us_;
        if (this->video_active_() && !this->blanked_ && this->light_->is_dithering() &&
            since < this->frame_interval_us_ * 5 / 8) {
#ifdef SYNCBOX_SATELLITES
          this->sat_hold_ = true;
#endif
          this->light_->refresh();
#ifdef SYNCBOX_SATELLITES
          this->sat_hold_ = false;
#endif
        }
      });
      // Planista przyjmuje nowy czasomierz dopiero na poczatku kolejnego obiegu petli, a petla spalaby do nastepnej
      // klatki: budzimy ja od razu, zeby zasnela juz tylko do chwili drugiego zapisu.
      App.wake_loop_threadsafe();
    }
    this->log_stats_();
    return;
  }

  // Interpolacja: nowa klatka staje sie celem; startem jest to, co jest teraz na tasmie.
  const size_t n3 = n * 3;
  for (size_t i = 0; i < n3; i++)
    this->interp_from_[i] = this->interp_done_ ? this->interp_to_[i] : this->interp_cur_[i];
  for (size_t s = 0; s < n; s++) {
    this->interp_to_[s * 3] = this->seg_colors_[s].r;
    this->interp_to_[s * 3 + 1] = this->seg_colors_[s].g;
    this->interp_to_[s * 3 + 2] = this->seg_colors_[s].b;
  }
  this->interp_done_ = false;
  this->last_interp_push_us_ = 0;
  this->light_->set_subframes(false);
  this->interpolate_tick_(now_us);
  this->log_stats_();
}

// Raz na sekunde, PO wyslaniu klatki na tasme (wydruk na UART trwa i opoznialby te klatke).
void SyncBox::log_stats_() {
  if ((!this->log_frames_ && !this->log_sections_) || this->frames_total_ - this->last_log_frames_ < 60)
    return;
  this->last_log_frames_ = this->frames_total_;
  const uint32_t t0 = micros();
  if (this->log_frames_) {
    const uint8_t *f = this->frame_buf_ + this->pix_off_;
    ESP_LOGI(TAG,
             "klatki: %u pełne, %u częściowe, %u zgubione, %u błędne; wideo %.1f fps, taśma %.1f fps; bajty: %02X %02X %02X %02X "
             "%02X %02X; seg0=%02X%02X%02X; log %u us",
             (unsigned) this->frames_total_, (unsigned) this->frames_partial_, (unsigned) this->frames_dropped_,
             (unsigned) this->frames_bad_, this->video_fps_, this->led_fps_, f[0], f[1], f[2], f[3], f[4], f[5], this->seg_colors_[0].r,
             this->seg_colors_[0].g, this->seg_colors_[0].b, (unsigned) this->log_us_);
#ifdef SYNCBOX_SATELLITES
    if (this->sat_us_cnt_ != 0) {
      ESP_LOGI(TAG, "satelity: %u pakietów, obsługa śr. %u us, max %u us, błędy wysyłki %u", (unsigned) this->sat_us_cnt_,
               (unsigned) (this->sat_us_sum_ / this->sat_us_cnt_), (unsigned) this->sat_us_max_, (unsigned) this->sat_errors_);
      this->sat_us_sum_ = this->sat_us_max_ = this->sat_us_cnt_ = 0;
    }
#endif
  }
  if (this->log_sections_)
    this->log_sections_line_();
  this->log_us_ = micros() - t0;
}

// Kolory sekcji bokami: L od dolu do gory, G od lewej do prawej, P od gory do dolu (jak indeksy w cell_range_).
void SyncBox::log_sections_line_() {
  char buf[400];
  int k = 0;
  static const char NAMES[4] = {'L', 'G', 'P', 'D'};
  for (uint8_t side = 0; side < 4; side++) {
    if (this->sides_[side] == 0)
      continue;
    k += snprintf(buf + k, sizeof(buf) - k, "%c:", NAMES[side]);
    for (uint8_t idx = 0; idx < this->sides_[side]; idx++) {
      for (size_t s = 0; s < this->seq_.size(); s++) {
        if (this->seq_[s].side != side || this->seq_[s].index != idx)
          continue;
        const Color c = this->seg_colors_[s];
        if (k < (int) sizeof(buf) - 8)
          k += snprintf(buf + k, sizeof(buf) - k, "%02X%02X%02X ", c.r, c.g, c.b);
        break;
      }
    }
  }
  ESP_LOGI(TAG, "sekcje %s kadr x %u..%u y %u..%u srodek %.0f%%", buf, this->crop_.x0, this->crop_.x1, this->crop_.y0,
           this->crop_.y1, this->inner_mix_ * 100.0f);
}

// Pas "srodka" dla sekcji: od wewnetrznej granicy jej prostokata przy krawedzi (wejscie = wynik cell_range_) do osi
// kadru, na szerokosci samej sekcji. false = pas za plytki (waski kadr, duza glebokosc probkowania).
bool SyncBox::inner_range_(uint8_t side, uint8_t &xa, uint8_t &xb, uint8_t &ya, uint8_t &yb) const {
  const int cx0 = this->crop_.x0, cx1 = this->crop_.x1, cy0 = this->crop_.y0, cy1 = this->crop_.y1;
  const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
  int a, b;
  switch (side) {
    case SIDE_LEFT:
      a = xb + 1;
      b = cx0 + cw / 2 - 1;
      break;
    case SIDE_RIGHT:
      a = cx1 - cw / 2 + 1;
      b = xa - 1;
      break;
    case SIDE_TOP:
      a = yb + 1;
      b = cy0 + ch / 2 - 1;
      break;
    default:  // SIDE_BOTTOM
      a = cy1 - ch / 2 + 1;
      b = ya - 1;
      break;
  }
  if (b - a + 1 < MIX_MIN_CELLS)
    return false;
  if (side == SIDE_LEFT || side == SIDE_RIGHT) {
    xa = (uint8_t) a;
    xb = (uint8_t) b;
  } else {
    ya = (uint8_t) a;
    yb = (uint8_t) b;
  }
  return true;
}

void SyncBox::push_colors_(const float *rgb) {
  const size_t n = this->seq_.size();
  for (size_t s = 0; s < n; s++) {
    this->seg_colors_[s] = Color((uint8_t) (rgb[s * 3] + 0.5f), (uint8_t) (rgb[s * 3 + 1] + 0.5f),
                                 (uint8_t) (rgb[s * 3 + 2] + 0.5f));
  }
  this->light_->push_video_f(rgb, this->seg_colors_.data(), n);
  this->blanked_ = false;
}

void SyncBox::interpolate_tick_(uint32_t now_us) {
  if (!this->video_active_()) {
    this->interp_done_ = true;
    return;
  }
  static const uint32_t TICK_US = 16000;  // ~60 Hz
  if (this->last_interp_push_us_ != 0 && (uint32_t) (now_us - this->last_interp_push_us_) < TICK_US)
    return;
  float t = (float) (uint32_t) (now_us - this->last_frame_us_) / (float) this->frame_interval_us_;
  if (t >= 1.0f)
    t = 1.0f;
  const size_t n3 = this->seq_.size() * 3;
  for (size_t i = 0; i < n3; i++)
    this->interp_cur_[i] = this->interp_from_[i] + (this->interp_to_[i] - this->interp_from_[i]) * t;
  this->push_colors_(this->interp_cur_.data());
  this->last_interp_push_us_ = now_us;
  if (t >= 1.0f)
    this->interp_done_ = true;
}
#else
void SyncBox::spi_task_() {}
bool SyncBox::parse_aux_(const uint8_t *) { return false; }
uint16_t SyncBox::build_dn_() { return 0; }
void SyncBox::link_loop_() {}
void SyncBox::console_loop_() {}
void SyncBox::console_close_(const char *) {}
void SyncBox::update_crop_(const uint8_t *, const uint8_t *, const uint8_t *) {}
void SyncBox::cell_range_(uint8_t, uint8_t, uint8_t &xa, uint8_t &xb, uint8_t &ya, uint8_t &yb) const {
  xa = xb = ya = yb = 0;
}
void SyncBox::process_frame_() {}
void SyncBox::log_stats_() {}
void SyncBox::log_sections_line_() {}
bool SyncBox::inner_range_(uint8_t, uint8_t &, uint8_t &, uint8_t &, uint8_t &) const { return false; }
#endif

// ------------------------------------------------------------- SyncBoxLight

void SyncBoxLight::setup() {
  if (this->num_leds_ == 0 && this->parent_ != nullptr)
    this->num_leds_ = this->parent_->total_segments();
  if (this->num_leds_ == 0)
    this->num_leds_ = 28;
  RAMAllocator<uint8_t> allocator;
  this->buf_ = allocator.allocate(this->num_leds_ * 3);
  this->effect_data_ = allocator.allocate(this->num_leds_);
  if (this->buf_ == nullptr || this->effect_data_ == nullptr) {
    ESP_LOGE(TAG, "Brak pamięci na bufor taśmy");
    this->mark_failed();
    return;
  }
  memset(this->buf_, 0, this->num_leds_ * 3);
  memset(this->effect_data_, 0, this->num_leds_);
}

light::ESPColorView SyncBoxLight::get_view_internal(int32_t index) const {
  uint8_t *led = this->buf_ + index * 3;
  return {led, led + 1, led + 2, nullptr, this->effect_data_ + index, &this->correction_};
}

void SyncBoxLight::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sync Box LED strip:\n"
                "  Sekcje: %u\n"
                "  Max. częstotliwość odświeżania: %u us",
                this->num_leds_, (unsigned) this->max_refresh_rate_);
}

void SyncBoxLight::write_state(light::LightState *state) {
  const uint32_t now = micros();
  if (this->max_refresh_rate_ != 0 && (uint32_t) (now - this->last_refresh_) < this->max_refresh_rate_) {
    this->schedule_show();  // sprobuj w nastepnej petli, zeby nie zgubic zmiany
    return;
  }
  this->last_refresh_ = now;
  this->mark_shown_();
  const size_t n3 = (size_t) this->num_leds_ * 3;
  if (this->video_f_ && this->dither_ && this->buf_ != nullptr && state != nullptr && this->vid_f_.size() >= n3 &&
      this->dith_err_.size() >= n3) {
    // Dithering w czasie: cel jest zaokraglany do 1/4 poziomu (gdy klatka idzie na tasme dwa razy w rownych odstepach; inaczej do 1/2),
    // a blad zaokraglenia do calego poziomu przechodzi na kolejny zapis. Najwolniejszy wzor ma wtedy ok. 30 Hz.
    const float br = state->current_values.get_brightness() * state->current_values.get_state();
    const float q = this->subframes_ ? 4.0f : 2.0f;
    for (size_t i = 0; i < n3; i++) {
      float v = this->vid_f_[i] * br;
      v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
      v = floorf(v * q + 0.5f) / q;
      const float acc = v + this->dith_err_[i];
      int o = (int) floorf(acc + 0.5f);
      o = o < 0 ? 0 : (o > 255 ? 255 : o);
      this->dith_err_[i] = acc - (float) o;
      this->buf_[i] = (uint8_t) o;
    }
  }
  if (this->parent_ != nullptr && this->buf_ != nullptr)
    this->parent_->transmit(this->buf_, this->num_leds_, state != nullptr && state->current_values.is_on());
}

void SyncBoxLight::push_video_f(const float *rgb, const Color *colors, size_t n) {
  if (!this->is_on())
    return;
  const size_t m = std::min<size_t>(n, this->num_leds_);
  if (this->vid_f_.size() < (size_t) this->num_leds_ * 3) {
    this->vid_f_.assign((size_t) this->num_leds_ * 3, 0.0f);
    this->dith_err_.assign((size_t) this->num_leds_ * 3, 0.0f);
  }
  for (size_t i = 0; i < m * 3; i++)
    this->vid_f_[i] = rgb[i];
  for (size_t i = 0; i < m; i++)
    (*this)[i] = colors[i];
  this->video_f_ = true;
  if (this->state_parent_ != nullptr) {
    this->write_state(this->state_parent_);
  } else {
    this->schedule_show();
  }
}

void SyncBoxLight::push_video(const Color *colors, size_t n) {
  this->video_f_ = false;  // kolory calkowite (np. wygaszenie): bez ditheringu
  if (!this->is_on())
    return;
  const size_t m = std::min<size_t>(n, this->num_leds_);
  for (size_t i = 0; i < m; i++)
    (*this)[i] = colors[i];
  // Bez schedule_show(): wysylamy od razu, zamiast czekac na kolejny obieg petli (do 16 ms).
  if (this->state_parent_ != nullptr) {
    this->write_state(this->state_parent_);
  } else {
    this->schedule_show();
  }
}

void SyncBoxLight::refresh() {
  if (this->state_parent_ == nullptr)
    return;
  this->write_state(this->state_parent_);
}

void SyncBoxLight::restore_static() {
  this->video_f_ = false;
  if (this->state_parent_ == nullptr)
    return;
  this->update_state(this->state_parent_);
  this->schedule_show();
}

}  // namespace esphome::syncbox

#endif  // USE_BK72XX
