// © Kay Sievers <kay@versioduo.com>, 2020-2023
// SPDX-License-Identifier: Apache-2.0

#include "MIDISong.h"
#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.glockenspiel-37", 69, "versioduo:samd:control");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(37, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2MIDI::SerialDevice MIDISerial(&SerialMIDI);
static V2Link::Port Socket(&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE);

// The button switches the state with a multi-click long-press.
static class Manual {
public:
  enum class Mode { Notes, Song, Test } mode{};
  Mode getMode() const {
    return _mode;
  }

  void setMode(Mode mode, float color = 0) {
    _mode = mode;

    switch (_mode) {
      case Mode::Notes:
        LED.reset();
        LED.setHSV(color, 1, 0.25);
        break;

      case Mode::Song:
        LED.reset();
        LED.setBrightness(0.25);
        break;

      case Mode::Test:
        LED.reset();
        LED.rainbow(1, 3, 0.4);
        break;
    }
  }

  void setColor(V2Color::Hue color) {
    LED.reset();
    LED.setHSV(color, 1, 0.25);
  }

  void splashColor(V2Color::Hue color) {
    LED.splashHSV(0.5, color, 1, 0.25);
  }

private:
  Mode _mode{};
} Manual;

static class Device : public V2Device {
public:
  constexpr Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 glockenspiel-37";
    metadata.description = "37 Bar Glockenspiel";
    metadata.home        = "https://versioduo.com/#glockenspiel-37";

    help.device = "Notes are controlled by a trigger and a damper, it allows a piano-like "
                  "velocity and tone duration control; MIDI Note-Off will cause the currently "
                  "playing tone to be damped.";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid          = 0xe910;
    usb.ports.access = 6;

    configuration = {.size{sizeof(config)}, .data{&config}};
  }

  enum class CC {
    Volume       = V2MIDI::CC::ChannelVolume,
    SustainPedal = V2MIDI::CC::SustainPedal,
    Color        = V2MIDI::CC::Controller14,
    Saturation   = V2MIDI::CC::Controller15,
    Brightness   = V2MIDI::CC::Controller89,
    Rainbow      = V2MIDI::CC::Controller90,
  };

  // 37 notes, C4 - C7. The Middle C is C3.
  static constexpr struct {
    uint8_t start;
    uint8_t count;
  } notes{.start{V2MIDI::C(4)}, .count{37}};

  // Config, written to EEPROM
  struct {
    // Velocity value offset to trigger a note. When no calibration is applied,
    // this is the minimum and maximum velocity to trigger a note.
    struct {
      uint8_t min;
      uint8_t max;
    } calibration[notes.count]{};

    // LED color.
    struct {
      uint8_t h{15};
      uint8_t s{40};
      uint8_t v{100};
    } color;
  } config;

  enum class Program : uint8_t {
    Standard,
    Damper,
    Dampened,
    Calibration,
    _count,
  };

  void setProgram(uint8_t channel, Program number) {
    _channels[channel].program = number;

    switch (Manual.getMode()) {
      case Manual::Mode::Notes:
        Manual.setColor(_programs[(uint8_t)_channels[channel].program].color);
        break;

      case Manual::Mode::Song:
      case Manual::Mode::Test:
        Manual.splashColor(_programs[(uint8_t)_channels[channel].program].color);
        break;
    }
  }

  void setSustain(uint8_t value) {
    _sustain = value;

    // Damp currenly playing notes.
    if (_sustain < 64) {
      for (uint8_t i = 0; i < notes.count; i++) {
        if (!_notes[i].playing)
          continue;

        if (getTriggerDuration(i) > 4.f)
          continue;

        _notes[i].playing = false;
        sendDamper(i, 2.5, 0.8, true, true);
      }
    }
  }

  void play(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (note < notes.start || note > (notes.start + notes.count - 1))
      return;

    touchTimeout();
    uint8_t index = note - notes.start;

    // Ignore the note when the same note with a higher priority is already playing.
    if (!_notesPriority[index].set(velocity == 0 ? -1 : velocity, channel))
      return;

    switch (_channels[channel].program) {
      case Program::Standard:
        playStandard(channel, index, velocity);
        break;

      case Program::Damper:
        playDamper(channel, index, velocity);
        break;

      case Program::Dampened:
        playDampened(channel, index, velocity);
        break;

      case Program::Calibration:
        playCalibration(channel, index, velocity);
        break;
    }
  }

  void allNotesOff() {
    if (_force.trigger()) {
      reset();
      return;
    }

    setDefaultValues();

    for (uint8_t i = 0; i < 1 + (notes.count / 8); i++) {
      V2MIDI::Packet _midi{};
      _midi.setPort(i);
      _midi.setControlChange(0, V2MIDI::CC::AllNotesOff);
      Socket.send(&_midi);
    }
  }

private:
  V2Music::ForcedStop _force;

  struct {
    uint32_t usec{};
    bool notes{};
  } _timeout;

  uint8_t _volume{100};
  uint8_t _sustain{};
  V2Music::Priority<16> _sustainPriority{};
  float _rainbow{};

  const struct {
    const char *name;
    V2Color::Hue color;
  } _programs[(uint8_t)Program::_count]{
    [(uint8_t)Program::Standard]    = {.name{"Standard"}, .color{V2Color::Orange}},
    [(uint8_t)Program::Damper]      = {.name{"Damper"}, .color{V2Color::Cyan}},
    [(uint8_t)Program::Dampened]    = {.name{"Dampened"}, .color{V2Color::Green}},
    [(uint8_t)Program::Calibration] = {.name{"Calibration"}, .color{V2Color::Magenta}},
  };

  struct {
    Program program{};
    uint16_t bank{};

    // LED color.
    struct {
      float h;
      float s;
      float v;
    } led{};
  } _channels[16];

  struct {
    uint32_t startUsec;
    bool playing;
  } _notes[notes.count]{};

  V2Music::Priority<16> _notesPriority[notes.count]{};

  void handleInit() override {
    if (usb.ports.enableAccess) {
      usb.midi.setPortName(1, "control");
      usb.midi.setPortName(2, "pulse 1");
      usb.midi.setPortName(3, "pulse 2");
      usb.midi.setPortName(4, "pulse 3");
      usb.midi.setPortName(5, "pulse 4");
      usb.midi.setPortName(6, "pulse 5");
    }
  }

  void touchTimeout() {
    _timeout.usec  = V2Base::getUsec();
    _timeout.notes = true;
  }

  void runTimeout() {
    if (_timeout.usec == 0)
      return;

    if (V2Base::getUsecSince(_timeout.usec) < 30 * 1000 * 1000)
      return;

    if (_timeout.notes) {
      resetNotes();
      _timeout.notes = false;
    }

    if (V2Base::getUsecSince(_timeout.usec) < 900 * 1000 * 1000)
      return;

    allNotesOff();
    _timeout.usec = 0;
  }

  void resetNotes() {
    for (uint8_t i = 0; i < notes.count; i++) {
      _notes[i] = {};
      _notesPriority[i].reset();
    }

    LEDExt.reset();
  }

  void setDefaultValues() {
    _timeout = {};
    _volume  = 100;
    _sustain = 0;
    _sustainPriority.reset();
    _rainbow = 0;

    for (uint8_t ch = 0; ch < 16; ch++) {
      _channels[ch].program = Program::Standard;
      _channels[ch].bank    = 0;

      _channels[ch].led.h = (float)config.color.h / 127.f * 360.f;
      _channels[ch].led.s = (float)config.color.s / 127.f;
      _channels[ch].led.v = (float)config.color.v / 127.f;
    }

    Manual.setMode(Manual::Mode::Notes, _programs[(uint8_t)_channels[0].program].color);
    resetNotes();
  }

  void handleReset() override {
    _force.reset();
    setDefaultValues();

    for (uint8_t i = 0; i < 1 + (notes.count / 8); i++) {
      V2MIDI::Packet _midi{};
      _midi.setPort(i);
      _midi.set(0, V2MIDI::Packet::Status::SystemReset);
      Socket.send(&_midi);
    }
  }

  void handleLoop() override {
    runTimeout();
  }

  void light(uint8_t channel, uint8_t note, float fraction) {
    if (fraction > 0.f) {
      const float brightness = 0.2f + (0.8f * fraction);
      LEDExt.setHSV(note, _channels[channel].led.h, _channels[channel].led.s, _channels[channel].led.v * brightness);
      led.flash(0.03, 0.3);

    } else
      LEDExt.setBrightness(note, 0);
  }

  // Seconds since the note is running.
  float getTriggerDuration(uint8_t index) {
    return (float)V2Base::getUsecSince(_notes[index].startUsec) / (1000.f * 1000.f);
  }

  // The pulse controllers are connected in reversed order.
  void getPulseAddress(uint8_t index, uint8_t &child, uint8_t &port) {
    child = ((notes.count - 1) - index) / 8;
    port  = (((notes.count - 1) - index) % 8) * 2;
  }

  void sendTrigger(uint8_t index, float watts, float seconds) {
    uint8_t child;
    uint8_t port;
    getPulseAddress(index, child, port);

    V2Link::Packet packet;
    V2Link::Packet::Pulse pulse{
      .port{port},
      .watts{watts},
      .seconds{seconds},
    };
    packet.setPulse(&pulse);
    Socket.send(child, &packet);
  }

  void sendDamper(uint8_t index, float watts, float seconds, bool fadeIn, bool fadeOut) {
    uint8_t child;
    uint8_t port;
    getPulseAddress(index, child, port);
    port++;

    V2Link::Packet packet;
    V2Link::Packet::Pulse pulse{
      .port{port},
      .watts{watts},
      .seconds{seconds},
      .fadeIn{fadeIn},
      .fadeOut{fadeOut},
    };
    packet.setPulse(&pulse);
    Socket.send(child, &packet);
  }

  void getPulse(float fraction, float &watts, float &seconds) {
    static constexpr struct {
      struct {
        float watts{0.8};
        float seconds{0.06};
      } min;
      struct {
        float watts{5};
        float seconds{0.01};
      } max;
    } range;

    watts = range.min.watts;
    watts += (range.max.watts - range.min.watts) * fraction;

    seconds = range.min.seconds;
    seconds += (range.max.seconds - range.min.seconds) * powf(fraction, 0.3);
  }

  float getFraction(uint8_t velocity) {
    const float fraction = (float)velocity / 127.f;
    return powf(fraction, 2);
  }

  float getFractionCalibrated(uint8_t index, uint8_t velocity) {
    const uint8_t min = config.calibration[index].min;
    const uint8_t max = config.calibration[index].max;

    // Default configuration, uncalibrated.
    if (min == 0 && max == 0)
      return getFraction(velocity);

    float floor = (float)min / 127.f;
    floor       = powf(floor, 2);

    float ceiling = (float)max / 127.f;
    ceiling       = powf(ceiling, 2);

    float fraction = (float)velocity / 127.f;
    fraction       = powf(fraction, 2);

    float range = (ceiling - floor) * fraction;
    return floor + range;
  }

  float adjustVolume(float fraction) {
    if (_volume < 100) {
      const float range = (float)_volume / 100.f;
      return fraction * range;
    }

    const float range = (float)(_volume - 100) / 27.f;
    return powf(fraction, 1 - (0.5f * range));
  }

  void playStandard(uint8_t channel, uint8_t index, uint8_t velocity) {
    if (velocity == 0) {
      // Damp only an active tone.
      if (getTriggerDuration(index) < 4.f) {
        float watts   = 1.5f + (0.5f * ((float)velocity / 127.f));
        float seconds = 1.5;

        if (_sustain < 120) {
          if (_sustain > 0) {
            const float fraction = (float)_sustain / 127.f;
            watts -= 0.8f * powf(fraction, 0.5);
            seconds -= 0.5f * powf(fraction, 0.5);
          }

          _notes[index].playing = false;
          sendDamper(index, watts, seconds, true, true);
        }
      }

      light(channel, index, 0);
      return;
    }

    //  Cancel a still active damper.
    sendDamper(index, 0, 0, false, false);

    if (_volume > 0) {
      float fraction = getFractionCalibrated(index, velocity);
      fraction       = adjustVolume(fraction);

      float watts;
      float seconds;
      getPulse(fraction, watts, seconds);

      // Record the time to decide if we need to damp the triggered note at NoteOff.
      _notes[index].startUsec = V2Base::getUsec();

      sendTrigger(index, watts, seconds);
      _notes[index].playing = true;
    }

    light(channel, index, (float)velocity / 127.f);
  }

  void playDamper(uint8_t channel, uint8_t index, uint8_t velocity) {
    if (velocity == 0) {
      light(channel, index, 0);
      return;
    }

    if (_volume > 0) {
      float fraction = getFraction(velocity);
      fraction       = adjustVolume(fraction);
      sendDamper(index, 1.f + (4.f * fraction), 0.05, false, false);
    }

    light(channel, index, (float)velocity / 127.f);
  }

  void playDampened(uint8_t channel, uint8_t index, uint8_t velocity) {
    if (velocity == 0) {
      light(channel, index, 0);
      return;
    }

    if (_volume > 0) {
      float fraction = getFractionCalibrated(index, velocity);
      fraction       = adjustVolume(fraction);

      float watts;
      float seconds;
      getPulse(fraction, watts, seconds);
      sendTrigger(index, watts, seconds);
      sendDamper(index, 2, 0.5, false, true);
    }

    light(channel, index, (float)velocity / 127.f);
  }

  void playCalibration(uint8_t channel, uint8_t index, uint8_t velocity) {
    if (velocity > 0) {
      float watts;
      float seconds;
      float fraction = getFraction(velocity);
      getPulse(fraction, watts, seconds);
      sendTrigger(index, watts, seconds);
    }

    light(channel, index, (float)velocity / 127.f);
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(channel, note, velocity);
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    play(channel, note, 0);
  }

  void handleProgramChange(uint8_t channel, uint8_t program) override {
    touchTimeout();

    if (program != V2MIDI::GM::Program::Glockenspiel)
      return;

    if (_channels[channel].bank >= (uint8_t)Program::_count)
      return;

    setProgram(channel, (Program)_channels[channel].bank);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    touchTimeout();

    // Controls for a specific channel.
    switch (controller) {
      case V2MIDI::CC::BankSelect:
        _channels[channel].bank = value << 7;
        return;

      case V2MIDI::CC::BankSelectLSB:
        _channels[channel].bank |= value;
        return;

      // Sustain is a global state, but the higher channels override the actual value.
      case (uint8_t)CC::SustainPedal:
        if (!_sustainPriority.set(value == 0 ? -1 : value, channel))
          return;

        // Restore the value from the lower priority.
        if (value == 0) {
          const int8_t sustain = _sustainPriority.get();
          if (sustain >= 0)
            value = sustain;
        }

        setSustain(value);
        return;

      case (uint8_t)CC::Color:
        _channels[channel].led.h = (float)value / 127.f * 360.f;
        break;

      case (uint8_t)CC::Saturation:
        _channels[channel].led.s = (float)value / 127.f;
        break;

      case (uint8_t)CC::Brightness:
        _channels[channel].led.v = (float)value / 127.f;
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        return;
    }

    if (channel != 0)
      return;

    // Controls for the main channel only.
    switch (controller) {
      case (uint8_t)CC::Volume:
        _volume = value;
        break;

      case (uint8_t)CC::Brightness:
        if (_rainbow > 0.f)
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _channels[0].led.v);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f)
          LEDExt.reset();

        else
          LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), _channels[0].led.v);
        break;
    }
  }

  void handleSystemReset() override {
    reset();
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "calibration";
      setting["title"]   = "Calibration";

      // Notes are sent on a special program which plays the raw uncalibrated values.
      JsonObject jsonProgram = setting["program"].to<JsonObject>();
      jsonProgram["number"]  = V2MIDI::GM::Program::Glockenspiel;
      jsonProgram["bank"]    = (uint8_t)Program::Calibration;

      JsonObject jsonChromatic = setting["chromatic"].to<JsonObject>();
      jsonChromatic["start"]   = notes.start;
      jsonChromatic["count"]   = notes.count;

      setting["path"] = "calibration";
    }

    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "color";
      setting["title"]   = "Light";
      setting["path"]    = "color";
    }
  }

  void exportConfiguration(JsonObject json) override {
    json["#calibration"]      = "The “Raw” velocity values to play a note with velocity 1 and 127";
    JsonArray jsonCalibration = json["calibration"].to<JsonArray>();
    for (uint8_t i = 0; i < notes.count; i++) {
      JsonObject note = jsonCalibration.add<JsonObject>();
      uint8_t min     = config.calibration[i].min;
      uint8_t max     = config.calibration[i].max;

      // The default values are all 0 when no configuration is stored.
      if (min == 0)
        min = 1;

      if (max == 0)
        max = 127;

      note["min"] = min;
      note["max"] = max;
    }

    {
      json["#color"]    = "The LED color. Hue, saturation, brightness, 0..127";
      JsonArray jsonLed = json["color"].to<JsonArray>();
      jsonLed.add(config.color.h);
      jsonLed.add(config.color.s);
      jsonLed.add(config.color.v);
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonArray jsonCalibration = json["calibration"];
    if (jsonCalibration) {
      for (uint8_t i = 0; i < notes.count; i++) {
        if (!jsonCalibration[i].isNull()) {
          uint8_t min = jsonCalibration[i]["min"];
          uint8_t max = jsonCalibration[i]["max"];

          // Limit
          if (min > 127)
            min = 127;

          if (max > 127)
            max = 127;

          if (min == 0)
            min = 1;

          if (max == 0)
            max = 127;

          // Invalid
          if (max < min)
            max = min;

          config.calibration[i].min = min;
          config.calibration[i].max = max;

        } else {
          config.calibration[i].min = 1;
          config.calibration[i].max = 127;
        }
      }
    }

    JsonArray jsonLed = json["color"];
    if (jsonLed) {
      uint8_t color = jsonLed[0];
      if (color > 127)
        color = 127;
      config.color.h     = color;
      _channels[0].led.h = (float)color / 127.f * 360.f;

      uint8_t saturation = jsonLed[1];
      if (saturation > 127)
        saturation = 127;
      config.color.s     = saturation;
      _channels[0].led.s = (float)saturation / 127.f;

      uint8_t brightness = jsonLed[2];
      if (brightness > 127)
        brightness = 127;
      config.color.v     = brightness;
      _channels[0].led.v = (float)brightness / 127.f;
    }
  }

  void exportInput(JsonObject json) override {
    JsonArray jsonChannels = json["channels"].to<JsonArray>();
    for (uint8_t ch = 0; ch < 16; ch++) {
      JsonObject jsonChannel = jsonChannels.add<JsonObject>();
      jsonChannel["number"]  = ch;

      JsonArray jsonPrograms = jsonChannel["programs"].to<JsonArray>();
      for (uint8_t i = 0; i < (uint8_t)Program::_count; i++) {
        JsonObject jsonProgram = jsonPrograms.add<JsonObject>();
        jsonProgram["name"]    = _programs[i].name;
        jsonProgram["number"]  = V2MIDI::GM::Program::Glockenspiel;
        jsonProgram["bank"]    = i;
        if (i == (uint8_t)_channels[ch].program)
          jsonProgram["selected"] = true;
      }

      JsonArray jsonControllers = jsonChannel["controllers"].to<JsonArray>();
      if (ch == 0) {
        {
          JsonObject jsonController = jsonControllers.add<JsonObject>();
          jsonController["name"]    = "Volume";
          jsonController["number"]  = (uint8_t)CC::Volume;
          jsonController["value"]   = _volume;
        }
        {
          JsonObject jsonController = jsonControllers.add<JsonObject>();
          jsonController["name"]    = "Sustain Pedal";
          jsonController["number"]  = (uint8_t)CC::SustainPedal;
          jsonController["value"]   = _sustain;
        }
      }

      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Hue";
        jsonController["number"]  = (uint8_t)CC::Color;
        jsonController["value"]   = (uint8_t)(_channels[ch].led.h / 360.f * 127.f);
      }
      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Saturation";
        jsonController["number"]  = (uint8_t)CC::Saturation;
        jsonController["value"]   = (uint8_t)(_channels[ch].led.s * 127.f);
      }
      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Brightness";
        jsonController["number"]  = (uint8_t)CC::Brightness;
        jsonController["value"]   = (uint8_t)(_channels[ch].led.v * 127.f);
      }

      if (ch == 0) {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Rainbow";
        jsonController["number"]  = (uint8_t)CC::Rainbow;
        jsonController["value"]   = (uint8_t)(_rainbow * 127.f);
      }

      {
        JsonObject jsonChromatic = jsonChannel["chromatic"].to<JsonObject>();
        jsonChromatic["start"]   = notes.start;
        jsonChromatic["count"]   = notes.count;
      }
    }
  }

  virtual void exportSystemMIDIFile(JsonObject json);

  void exportSystem(JsonObject json) override {
    exportSystemMIDIFile(json);
  }
} Device;

// Dispatch MIDI packets.
static class MIDI {
public:
  void loop() {
    if (Device.usb.midi.receive(&_midi)) {
      if (_midi.getPort() == 0) {
        Device.dispatch(&Device.usb.midi, &_midi);

      } else {
        _midi.setPort(_midi.getPort() - 1);
        Socket.send(&_midi);
      }
    }

    if (MIDISerial.receive(&_midi))
      Device.dispatch(&Device.usb.midi, &_midi);
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets.
static class Link : public V2Link {
public:
  constexpr Link() : V2Link(NULL, &Socket) {
    Device.link = this;
  }

private:
  V2MIDI::Packet _midi{};

  // Forward children device events to the host.
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();
      if (address == 0x0f)
        return;

      if (Device.usb.midi.connected()) {
        if (!packet->receive(&_midi))
          return;

        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

static class MIDIFile : public V2MIDI::File::Tracks {
public:
  constexpr MIDIFile() : V2MIDI::File::Tracks(MIDISong) {}

  bool handleSend(uint16_t track, V2MIDI::Packet *packet) {
    Device.dispatch(&Device.usb.midi, packet);
    return true;
  }

  void handleStateChange(V2MIDI::File::Tracks::State state) {
    switch (state) {
      case V2MIDI::File::Tracks::State::Stop:
        Device.reset();
        break;
    }
  }
} MIDIFile;

void Device::exportSystemMIDIFile(JsonObject json) {
  JsonObject jsonTrack = json["track"].to<JsonObject>();
  char s[128];
  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Title, s, sizeof(s)) > 0)
    jsonTrack["title"] = s;

  if (MIDIFile.copyTag(V2MIDI::File::Event::Meta::Copyright, s, sizeof(s)) > 0)
    jsonTrack["creator"] = s;
}

static class Test {
public:
  static constexpr struct {
    uint8_t min;
    uint8_t step;
  } config{.min{1}, .step{15}};

  void stop() {
    if (_enabled)
      Device.reset();

    _enabled = false;
  }

  void play() {
    LEDExt.reset();
    LEDExt.rainbow(2, 2, 1);

    _resetUsec = V2Base::getUsec();
    _enabled   = true;
    _velocity  = config.min;
    _note      = 0;
    _usec      = 0;
  }

  void loop() {
    if (!_enabled)
      return;

    playNote();
  }

private:
  bool _enabled{};
  uint8_t _velocity{};
  uint8_t _note{};
  uint32_t _usec{};
  uint32_t _resetUsec{};

  void playNote() {
    if (_resetUsec > 0) {
      // Wait for the controllers to initialize after a reset.
      if (V2Base::getUsecSince(_resetUsec) < 500 * 1000)
        return;

      _resetUsec = 0;
    }

    if (V2Base::getUsecSince(_usec) < 200 * 1000)
      return;

    _usec = V2Base::getUsec();

    if (_note == 0) {
      _note = Device::notes.start;
      Device.play(0, _note, _velocity);

    } else if (_note < Device.notes.start + Device.notes.count - 1) {
      Device.play(0, _note, 0);
      _note++;
      Device.play(0, _note, _velocity);

    } else {
      _note = 0;

      _velocity += config.step;
      if (_velocity > 127)
        stop();
    }
  }
} TestMode;

static class Button : public V2Buttons::Button {
public:
  constexpr Button(uint8_t pin) : V2Buttons::Button(&_config, pin) {}

private:
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleClick(uint8_t count) override {
    switch (count) {
      case 0:
        MIDIFile.stop();
        TestMode.stop();
        Device.reset();
        break;

      case 1 ... static_cast<uint8_t>(Device::Program::_count):
        Device.setProgram(0, static_cast<Device::Program>(count - 1));
        break;
    }
  }

  void handleHold(uint8_t count) override {
    switch (count) {
      case 0:
        Device.reset();
        Manual.setMode(Manual::Mode::Song);
        MIDIFile.play();
        break;

      case 1:
        Device.reset();
        Manual.setMode(Manual::Mode::Test);
        TestMode.play();
        break;
    }
  }
} Buttons[]{
  Button(PIN_BUTTON),
  Button(PIN_BUTTON_REVISION_0),
};

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);
  LEDExt.begin();
  LEDExt.setDirection(true);
  LEDExt.setMaxBrightness(0.75);
  Device.begin();

  for (uint8_t i = 0; i < V2Base::countof(Buttons); i++)
    Buttons[i].begin();

  Link.begin();

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialSocket, 2);

  SerialMIDI.begin(31250);
  SerialMIDI.setTimeout(1);
  Device.serial = &MIDISerial;

  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Device.loop();

  switch (Manual.getMode()) {
    case Manual::Mode::Song:
      MIDIFile.loop();
      break;

    case Manual::Mode::Test:
      TestMode.loop();
      break;
  }

  if (Link.idle() && Device.idle())
    Device.sleep();
}
