#pragma once

#ifdef USE_BK72XX

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/color.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/addressable_light_effect.h"
#include "esphome/components/light/esp_color_view.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#if defined(SYNCBOX_SATELLITES) || defined(SYNCBOX_CONSOLE)
#include "esphome/components/socket/socket.h"
#endif

namespace esphome::syncbox {

enum SyncBoxMode : uint8_t { MODE_LIGHT = 0, MODE_VIDEO = 1 };
enum Side : uint8_t { SIDE_LEFT = 0, SIDE_TOP = 1, SIDE_RIGHT = 2, SIDE_BOTTOM = 3 };
// Co dostaje satelita (inne swiatlo w sieci, po E1.31/sACN):
enum SatelliteMode : uint8_t {
  SAT_AVERAGE = 0,  // sredni kolor calej tasmy
  SAT_MIRROR = 1,   // kopia wszystkich sekcji 1:1
  SAT_LEFT = 2,     // sredni kolor lewego boku
  SAT_TOP = 3,      // ... gornego
  SAT_RIGHT = 4,    // ... prawego
  SAT_BOTTOM = 5,   // ... dolnego
  SAT_FULL = 6,     // cala siatka wideo (16x16 = 256 px, 170 px na universe -> universe, universe+1)
  SAT_ROOM = 7,     // obwod pokoju: kolory krawedzi ekranu rozlozone po odcinkach tasmy lampy (segs)
};

class SyncBoxLight;

struct Segment {
  uint8_t side;
  uint8_t index;
};

struct Crop {
  uint8_t x0, y0, x1, y1;
  bool operator==(const Crop &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
  bool operator!=(const Crop &o) const { return !(*this == o); }
};

#ifdef SYNCBOX_SATELLITES
// Odcinek tasmy satelity w trybie SAT_ROOM: `leds` diod pokazuje obwod ekranu od u0 do u1.
// u: 0..1 lewy bok (dol -> gora), 1..2 gorna krawedz (lewo -> prawo), 2..3 prawy bok (gora -> dol),
//    3..4 dolna krawedz (prawo -> lewo); u0 > u1 = odcinek biegnie w przeciwna strone.
struct SatSegment {
  float u0, u1;
  uint16_t leds;
};

// Jedna dioda satelity w trybie SAT_ROOM: mieszanka dwoch sekcji tasmy TV (pozycje pa, pb; udzial drugiej fw/256).
struct SatTap {
  uint8_t pa, pb;
  uint16_t fw;
};

struct Satellite {
  std::string name;
  std::string address;  // IP odbiorcy; pusty = multicast 239.255.<uni_hi>.<uni_lo>
  uint16_t universe{1};
  uint8_t mode{SAT_AVERAGE};
  uint16_t leds{1};  // ile pikseli RGB wyslac (average/boki); mirror = liczba sekcji
  float brightness{1.0f};
  uint32_t min_interval_us{33333};
  uint32_t last_tx_us{0};
  uint8_t seq{0};
  struct sockaddr_storage addr;
  socklen_t addrlen{0};
  bool resolved{false};
  bool failed{false};
  std::vector<SatSegment> segs;  // tylko SAT_ROOM
  std::vector<SatTap> taps;      // tylko SAT_ROOM: wagi diod, liczone raz przy pierwszej wysylce
  uint32_t tau_us{0};            // stala czasowa wygladzania (0 = bez)
  float gamma{1.0f};             // gamma odbiornika (1 = wysylka wprost)
  std::vector<int32_t> state;    // wygladzony kolor diod, 1/256 poziomu
  std::vector<uint8_t> enc;      // kodowanie gamma: indeks = poziom x 4 (0..1020)
  bool prepared{false};
  uint8_t blur{0};               // SAT_ROOM: promien rozmycia wzdluz tasmy lampy (diody), dwa przebiegi
  bool closed{false};            // SAT_ROOM: tasma lampy tworzy zamknieta petle
};
#endif

class SyncBox : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  // ---- konfiguracja (codegen) ----
  void set_enable_pin(GPIOPin *pin) { this->enable_pin_ = pin; }
  void set_sides(uint8_t l, uint8_t t, uint8_t r, uint8_t b) {
    this->sides_[0] = l;
    this->sides_[1] = t;
    this->sides_[2] = r;
    this->sides_[3] = b;
  }
  void set_start_corner(uint8_t c) { this->start_corner_ = c; }
  void set_clockwise(bool cw) { this->clockwise_ = cw; }
  void set_color_order(uint8_t a, uint8_t b, uint8_t c) {
    this->order_[0] = a;
    this->order_[1] = b;
    this->order_[2] = c;
  }
  void set_reset_time(uint32_t us) { this->reset_us_ = us; }
  void set_refresh_interval(uint32_t ms) { this->refresh_interval_ms_ = ms; }
  void set_video_enabled(bool v) { this->video_enabled_ = v; }
  void set_spi_mode(uint8_t m) { this->spi_mode_ = m; }
  void set_frame_size(uint16_t s) { this->frame_size_ = s; }
  void set_grid(uint8_t w, uint8_t h) {
    this->grid_w_ = w;
    this->grid_h_ = h;
  }
  void set_pixel_order(uint8_t a, uint8_t b, uint8_t c) {
    this->pix_order_[0] = a;
    this->pix_order_[1] = b;
    this->pix_order_[2] = c;
  }
  void set_depth(uint8_t d) { this->depth_ = d == 0 ? 1 : d; }
  void set_smoothing(float s) { this->smoothing_ = clamp(s, 0.0f, 0.99f); }
  void set_saturation(float s) { this->saturation_ = clamp(s, 0.0f, 3.0f); }
  void set_video_brightness(float b) { this->video_brightness_ = clamp(b, 0.0f, 1.0f); }
  // Kalibracja kolorow obrazu (przed nasyceniem i jasnoscia):
  // poziomy wejsciowe 0..255 rozciagane do pelnej skali (czern ponizej black -> 0, biel powyzej white -> 255)
  void set_video_levels(uint8_t black, uint8_t white) {
    this->level_black_ = black;
    this->level_white_ = white > black ? white : (uint8_t) (black + 1);
    this->lut_dirty_ = true;
  }
  // balans bieli: mnozniki kanalow R/G/B (1.0 = bez zmian; niebieskawa biel -> blue ~0.85)
  void set_white_balance(float r, float g, float b) {
    this->wb_[0] = clamp(r, 0.0f, 2.0f);
    this->wb_[1] = clamp(g, 0.0f, 2.0f);
    this->wb_[2] = clamp(b, 0.0f, 2.0f);
    this->lut_dirty_ = true;
  }
  // gamma obrazu: 1.0 = bez zmian, 1.6-2.2 = wiekszy kontrast (ciemniejsze polcienie, zywsze kolory)
  void set_video_gamma(float g) {
    this->video_gamma_ = clamp(g, 0.3f, 4.0f);
    this->lut_dirty_ = true;
  }
  // Wylaczenie wykrywania pasow przywraca pelny kadr (inaczej zostawal ostatni wykryty).
  void set_letterbox_detection(bool b) {
    this->letterbox_ = b;
    if (!b) {
      this->crop_ = Crop{0, 0, (uint8_t) (this->eff_w_ - 1), (uint8_t) (this->eff_h_ - 1)};
      this->pending_crop_ = this->crop_;
      this->pending_count_ = 0;
    }
  }
  // Doswietlenie ze srodka obrazu: 0 = wylaczone, 0.25 = sekcja swieci co najmniej 25 % swiatla obrazu w glebi.
  void set_inner_mix(float s) {
    this->inner_mix_ = clamp(s, 0.0f, 0.5f);
    if (this->inner_mix_ <= 0.0f)
      std::fill(this->mix_v_.begin(), this->mix_v_.end(), 0.0f);
  }
  float get_inner_mix() const { return this->inner_mix_; }
  // Raz na sekunde kolory wszystkich sekcji w logu (do testow na stanowisku).
  void set_log_sections(bool b) { this->log_sections_ = b; }
  // Pojedyncze skladowe kalibracji (suwaki w HA ustawiaja je niezaleznie, takze przy starcie).
  void set_wb_channel(uint8_t c, float v) {
    if (c < 3 && v == v) {
      this->wb_[c] = clamp(v, 0.0f, 2.0f);
      this->lut_dirty_ = true;
    }
  }
  void set_video_black(float v) {
    if (v == v)
      this->set_video_levels((uint8_t) clamp(v, 0.0f, 200.0f), this->level_white_);
  }
  void set_video_white(float v) {
    if (v == v)
      this->set_video_levels(this->level_black_, (uint8_t) clamp(v, 1.0f, 255.0f));
  }
  void set_signal_timeout(uint32_t ms) { this->signal_timeout_ms_ = ms; }
  void set_auto_off(bool b) { this->auto_off_ = b; }
  void set_log_frames(bool b) { this->log_frames_ = b; }
  void set_interpolation(bool b) { this->interpolate_ = b; }
  bool get_interpolation() const { return this->interpolate_; }
  void set_side_offset(uint8_t side, uint8_t cells) {
    if (side < 4)
      this->side_offset_[side] = cells;
  }
  void set_enable_follows_light(bool b) { this->enable_follows_light_ = b; }
  void set_light(SyncBoxLight *l) { this->light_ = l; }
#ifdef USE_SENSOR
  void set_video_fps_sensor(sensor::Sensor *s) { this->video_fps_sensor_ = s; }
  void set_led_fps_sensor(sensor::Sensor *s) { this->led_fps_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_signal_sensor(binary_sensor::BinarySensor *b) { this->signal_sensor_ = b; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_average_color_sensor(text_sensor::TextSensor *t) { this->avg_sensor_ = t; }
#endif
#ifdef SYNCBOX_SATELLITES
  void add_satellite(const std::string &name, const std::string &address, uint16_t universe, uint8_t mode,
                     uint16_t leds, float brightness, uint8_t max_fps, uint32_t transition_ms, float gamma);
  // odcinek ostatnio dodanego satelity (tryb SAT_ROOM), w kolejnosci diod lampy
  void add_satellite_segment(float u0, float u1, uint16_t leds);
  void set_satellite_blur(uint8_t radius, bool closed);
#endif

  // ---- runtime ----
  // Tryb ustawia efekt swiatla "Synchronizacja HDMI" (SyncBoxVideoEffect): start = MODE_VIDEO, stop = MODE_LIGHT.
  void set_mode(SyncBoxMode m);
  SyncBoxMode get_mode() const { return this->mode_; }
  void set_enable(bool on);
  bool is_enabled() const { return this->enabled_; }
  // Satelity: wlacz/wylacz wysylke (po wylaczeniu satelity gasna)
  void set_satellites_enabled(bool b);
  bool get_satellites_enabled() const { return this->satellites_enabled_; }
  Color get_average_color() const { return this->avg_color_; }
  uint16_t total_segments() const {
    return this->sides_[0] + this->sides_[1] + this->sides_[2] + this->sides_[3];
  }
  bool has_signal() const { return this->signal_; }
  float get_video_fps() const { return this->video_fps_; }
  float get_led_fps() const { return this->led_fps_; }
  uint32_t get_frames_dropped() const { return this->frames_dropped_; }
  uint8_t get_depth() const { return this->depth_; }
  float get_smoothing() const { return this->smoothing_; }
  float get_saturation() const { return this->saturation_; }
  float get_video_brightness() const { return this->video_brightness_; }

  // wywolywane przez SyncBoxLight: rgb = R,G,B na piksel (po korekcji ESPHome);
  // light_on = stan wlaczenia swiatla, steruje zasilaniem tasmy (P24)
  void transmit(const uint8_t *rgb, size_t num_leds, bool light_on);

  // cialo watku SPI (publiczne tylko dla trampoliny C)
  void spi_task_();

  // ---- lacze z ESP32 poza obrazem: stan wejscia HDMI, konsola ESP32 po sieci ----
#ifdef USE_TEXT_SENSOR
  void set_hdmi_input_sensor(text_sensor::TextSensor *t) { this->hdmi_input_sensor_ = t; }
#endif
#ifdef USE_SENSOR
  void set_hdmi_dropouts_sensor(sensor::Sensor *s) { this->hdmi_dropouts_sensor_ = s; }
#endif
  void set_console(uint16_t port, const std::string &password) {
    this->console_port_ = port;
    this->console_password_ = password;
  }

 protected:
  void build_sequence_();
  void process_frame_();
  void update_crop_(const uint8_t *lum_rows, const uint8_t *lum_cols, const uint8_t *lum_cols_mid);
  void push_black_();
  void interpolate_tick_(uint32_t now_us);
  void push_colors_(const float *rgb);
  void cell_range_(uint8_t side, uint8_t index, uint8_t &xa, uint8_t &xb, uint8_t &ya, uint8_t &yb) const;
  bool inner_range_(uint8_t side, uint8_t &xa, uint8_t &xb, uint8_t &ya, uint8_t &yb) const;
  void log_sections_line_();
  void log_stats_();
  void update_average_(const uint8_t *rgb, size_t num_leds, bool light_on);
  void rebuild_lut_();
  bool video_active_() const;
#ifdef SYNCBOX_SATELLITES
  void send_satellites_(const uint8_t *rgb, size_t num_leds, bool light_on, bool force);
  bool send_e131_(Satellite &sat, const uint8_t *data, size_t channels, uint16_t universe_offset = 0);
  bool ring_tap_(float u, SatTap &tap) const;
  void prepare_satellite_(Satellite &sat);
#endif

  // piny / wyjscie
  GPIOPin *enable_pin_{nullptr};
  bool enabled_{false};
  bool enable_follows_light_{true};
  uint8_t order_[3]{2, 0, 1};  // BRG
  uint32_t reset_us_{300};
  std::vector<uint8_t> tx_buf_;
  uint32_t last_tx_end_us_{0};
  uint32_t last_tx_ms_{0};
  uint32_t refresh_interval_ms_{1000};  // 0 = bez okresowego odswiezania
  bool uart_ready_{false};

  // uklad tasmy
  uint8_t sides_[4]{7, 14, 7, 0};
  uint8_t start_corner_{0};
  bool clockwise_{true};
  std::vector<Segment> seq_;

  // wideo
  bool video_enabled_{true};
  uint8_t spi_mode_{0};
  uint16_t frame_size_{768};
  uint8_t grid_w_{16};
  uint8_t grid_h_{16};
  // Uklad pola w biezacej klatce: z naglowka ramki (ESP32 v10) albo z konfiguracji (ramka bez naglowka).
  uint8_t eff_w_{16};
  uint8_t eff_h_{16};
  uint16_t pix_off_{0};  // poczatek danych pola w ramce (16 przy naglowku)
  uint8_t pix_order_[3]{0, 1, 2};
  uint8_t depth_{3};
  uint8_t side_offset_[4]{0, 0, 0, 0};
  float smoothing_{0.5f};
  float saturation_{1.3f};
  float video_brightness_{1.0f};
  uint8_t level_black_{0};
  uint8_t level_white_{255};
  float wb_[3]{1.0f, 1.0f, 1.0f};
  float video_gamma_{1.0f};
  uint8_t lut_[3][256];
  float lutf_[3][256];  // to samo co lut_, ale bez zaokraglania do 8 bitow (ciemne kolory)
  bool lut_dirty_{true};
  bool letterbox_{true};
  uint32_t signal_timeout_ms_{1000};
  bool auto_off_{true};
  bool log_frames_{false};
  bool log_sections_{false};
  uint32_t log_us_{0};          // czas ostatniego wydruku statystyk (us) - czy log blokuje petle
  uint32_t last_log_frames_{0};
  uint8_t headerless_run_{0};   // kolejne ramki bez poprawnego naglowka (po ramkach z naglowkiem)
  // doswietlenie ze srodka: udzial i wygladzona jasnosc srodka kazdej sekcji
  float inner_mix_{0.0f};
  std::vector<float> mix_v_;
  // interpolacja miedzy klatkami: tasma odswiezana ~60 Hz takze przy wolnym zrodle
  bool interpolate_{true};
  std::vector<float> interp_from_;
  std::vector<float> interp_to_;
  std::vector<float> interp_cur_;
  uint32_t frame_interval_us_{100000};
  uint32_t last_frame_us_{0};
  uint32_t last_interp_push_us_{0};
  bool interp_done_{true};

  uint8_t *rx_buf_{nullptr};
  uint8_t *frame_buf_{nullptr};
  volatile bool frame_ready_{false};
  volatile uint32_t frames_total_{0};
  volatile uint32_t frames_partial_{0};
  volatile uint32_t frames_dropped_{0};
  uint32_t frames_bad_{0};  // klatki z niezgodna suma kontrolna (blad transmisji SPI)
  volatile uint32_t last_frame_ms_{0};
  uint32_t frames_out_{0};
  void *spi_thread_{nullptr};

  // ---- lacze z ESP32 (opis formatu: esp32/main/link.h) ----
  // Bufor pierscieniowy: jeden watek pisze, drugi czyta; liczniki rosna bez konca, indeks = licznik % rozmiar.
  struct Ring {
    uint8_t *b{nullptr};
    uint32_t size{0};
    volatile uint32_t w{0}, r{0};
    uint32_t avail() const { return this->w - this->r; }
    uint32_t space() const { return this->size - (this->w - this->r); }
    void write(const uint8_t *d, uint32_t n) {
      for (uint32_t i = 0; i < n; i++)
        this->b[(this->w + i) % this->size] = d[i];
      this->w = this->w + n;
    }
    uint32_t read(uint8_t *d, uint32_t max) {
      uint32_t n = this->avail();
      if (n > max)
        n = max;
      for (uint32_t i = 0; i < n; i++)
        d[i] = this->b[(this->r + i) % this->size];
      this->r = this->r + n;
      return n;
    }
    void drop_all() { this->r = this->w; }  // tylko strona czytajaca
  };
  static const uint16_t AUX_LEN = 512;
  static const uint16_t DN_MAX = 1024;
  static const uint16_t DN_SEND = 1044;  // 8 bajtow zerowych + 9 naglowka + DN_MAX + 2 CRC, zaokraglone do 4
  bool parse_aux_(const uint8_t *aux);
  uint16_t build_dn_();
  void link_loop_();
  void console_loop_();
  void console_close_(const char *why);
  uint8_t aux_raw_[24]{};              // ostatni stan z ESP32; pisze watek SPI
  volatile uint32_t aux_gen_{0};       // parzysty = aux_raw_ spojny
  volatile uint32_t aux_seen_ms_{0};
  volatile uint32_t aux_frames_{0}, aux_bad_{0};
  uint8_t *dn_buf_{nullptr};           // blok zwrotny (MISO)
  uint8_t dn_pay_[DN_MAX]{};           // porcja w dol czekajaca na potwierdzenie
  uint16_t dn_len_{0};
  uint8_t dn_seq_{0}, up_last_{0};
  volatile bool tun_active_{false}, tun_resetting_{false};
  bool reset_applied_{false};
  Ring con_up_, con_dn_;               // ESP32 -> klient (pisze watek SPI), klient -> ESP32 (pisze petla)
  uint16_t console_port_{0};
  std::string console_password_;
#ifdef SYNCBOX_CONSOLE
  std::unique_ptr<socket::Socket> con_server_, con_client_;
#endif
  bool con_server_failed_{false}, con_authed_{false};
  uint32_t con_reset_ms_{0};
  std::string con_auth_buf_;
  // to, co petla wie o ESP32 (kopia aux_raw_)
  bool esp_fresh_{false};
  uint8_t esp_flags_{0};
  uint8_t esp_ver_logged_{0};
  uint16_t esp_dropouts_{0};
  bool esp_dropouts_known_{false};
  std::string hdmi_published_;
  uint32_t link_pub_ms_{0};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *hdmi_input_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *hdmi_dropouts_sensor_{nullptr};
#endif
  bool spi_started_{false};

  Crop crop_{0, 0, 15, 15};
  Crop pending_crop_{0, 0, 15, 15};
  uint8_t pending_count_{0};
  std::vector<float> smooth_;
  std::vector<Color> seg_colors_;

  // stan
  SyncBoxMode mode_{MODE_LIGHT};
  bool signal_{false};
  uint32_t signal_start_ms_{0};  // poczatek biezacej serii ramek (histereza czujnika sygnalu)
  // "sa ramki": liczone z licznika ramek i zegara petli glownej (bez znacznika czasu z watku SPI)
  uint32_t sig_seen_frames_{0};
  uint32_t sig_seen_ms_{0};
  bool sig_stale_{true};
  bool blanked_{false};
  uint32_t last_stats_ms_{0};
  uint32_t stats_frames_in_{0};
  uint32_t stats_frames_out_{0};
  float video_fps_{0};
  float led_fps_{0};
  SyncBoxLight *light_{nullptr};
#ifdef USE_SENSOR
  sensor::Sensor *video_fps_sensor_{nullptr};
  sensor::Sensor *led_fps_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *signal_sensor_{nullptr};
#endif

  // sredni kolor (dla automatyzacji HA, np. zarowek Tuya, ktore nie znaja E1.31)
  Color avg_color_{Color::BLACK};
  Color avg_published_{Color::BLACK};
  uint32_t avg_published_ms_{0};
  bool avg_dirty_{true};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *avg_sensor_{nullptr};
#endif

  // satelity (E1.31 / sACN)
  bool satellites_enabled_{true};
#ifdef SYNCBOX_SATELLITES
  std::vector<Satellite> satellites_;
  std::unique_ptr<socket::Socket> udp_;
  uint8_t e131_cid_[16]{};
  uint32_t sat_packets_{0};
  uint32_t sat_errors_{0};
  std::vector<uint8_t> side_pos_[4];  // [bok][indeks na boku] -> pozycja sekcji na tasmie TV
  uint32_t sat_log_ms_{0};
  // ostatni bufor tasmy czekajacy na wysylke do satelitow (wysylka idzie z petli, nie z transmit())
  std::vector<uint8_t> sat_rgb_;
  bool sat_pending_{false};
  bool sat_hold_{false};  // trwa zapis posredni tasmy: bez wysylki do satelitow
  uint32_t sat_us_sum_{0}, sat_us_max_{0}, sat_us_cnt_{0};  // czas obslugi jednego satelity (liczenie + UDP)
#endif
};

class SyncBoxLight : public light::AddressableLight {
 public:
  void setup() override;
  void write_state(light::LightState *state) override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  int32_t size() const override { return this->num_leds_; }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  void clear_effect_data() override {
    if (this->effect_data_ != nullptr)
      memset(this->effect_data_, 0, this->num_leds_);
  }

  void set_parent(SyncBox *p) { this->parent_ = p; }
  void set_num_leds(uint16_t n) { this->num_leds_ = n; }
  void set_max_refresh_rate(uint32_t us) { this->max_refresh_rate_ = us; }

  bool is_on() const { return this->state_parent_ != nullptr && this->state_parent_->current_values.is_on(); }
  // Wpisuje kolory wideo do bufora (z korekcja i jasnoscia swiatla) i planuje wyslanie.
  void push_video(const Color *colors, size_t n);
  // Jak wyzej, ale z kolorami ulamkowymi (0..255, R,G,B na sekcje). Czesc ulamkowa jest oddawana przez
  // przelaczanie sasiednich poziomow w kolejnych zapisach tasmy (dithering w czasie), dzieki czemu ciemne
  // kolory nie skacza miedzy poziomami 1, 2, 3 i zachowuja odcien.
  void push_video_f(const float *rgb, const Color *colors, size_t n);
  void set_dithering(bool on) { this->dither_ = on; }
  bool is_dithering() const { return this->dither_ && this->video_f_; }
  bool get_dithering() const { return this->dither_; }
  // true = hub wysyla kazda klatke dwa razy w rownych odstepach (drugi zapis w polowie okresu): krok 1/4 poziomu
  void set_subframes(bool two) { this->subframes_ = two; }
  // Po wyjsciu z trybu wideo: przywraca kolor statyczny swiatla.
  void restore_static();
  // Ponowne wyslanie aktualnego bufora (okresowe odswiezanie tasmy).
  void refresh();

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;

  uint8_t *buf_{nullptr};          // R,G,B na piksel
  uint8_t *effect_data_{nullptr};
  SyncBox *parent_{nullptr};
  uint16_t num_leds_{0};
  uint32_t last_refresh_{0};
  uint32_t max_refresh_rate_{0};
  bool dither_{true};
  bool subframes_{false};
  bool video_f_{false};            // bufor vid_f_ jest aktualny (tryb wideo)
  std::vector<float> vid_f_;       // kolory wideo przed zaokragleniem
  std::vector<float> dith_err_;    // blad zaokraglenia przenoszony do nastepnego zapisu
};

// Efekt swiatla "Synchronizacja HDMI": dopoki jest aktywny, kolory sekcji pochodza z obrazu.
// Sam efekt nic nie rysuje - klatki trafiaja do bufora swiatla bezposrednio z hubu (push_video).
class SyncBoxVideoEffect : public light::AddressableLightEffect {
 public:
  explicit SyncBoxVideoEffect(const char *name) : AddressableLightEffect(name) {}
  void set_parent(SyncBox *p) { this->parent_ = p; }
  void start() override {
    if (this->parent_ != nullptr)
      this->parent_->set_mode(MODE_VIDEO);
  }
  void stop() override {
    AddressableLightEffect::stop();
    if (this->parent_ != nullptr)
      this->parent_->set_mode(MODE_LIGHT);
  }
  // Bazowe apply() po kazdym obiegu petli woła schedule_show() -> dodatkowy zapis tasmy co 16 ms (120 fps zamiast 60).
  // Klatki trafiaja na tasme bezposrednio z hubu, wiec efekt sam nic nie odswieza.
  void apply() override {}
  void apply(light::AddressableLight &it, const Color &current_color) override {}

 protected:
  SyncBox *parent_{nullptr};
};

}  // namespace esphome::syncbox

#endif  // USE_BK72XX
