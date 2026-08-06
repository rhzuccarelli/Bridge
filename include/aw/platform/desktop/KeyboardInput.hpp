#pragma once

#include <memory>

#include "aw/core/Controls.hpp"
#include "aw/hal/ButtonInterface.hpp"
#include "aw/hal/EncoderInterface.hpp"

namespace aw::platform::desktop {

/// Key codes for keys that are not a single character.
/// Chosen above 0xFF so they cannot collide with an ASCII byte.
enum : int {
    kKeyUp = 0x100,
    kKeyDown,
    kKeyLeft,
    kKeyRight,
    kKeyEnter,
    kKeyEscape,
    kKeyBackspace,
};

enum class KeyAction : std::uint8_t {
    None,        ///< Unmapped key; ignored.
    Button,      ///< Press `button`.
    EncoderCw,   ///< One detent clockwise.
    EncoderCcw,  ///< One detent counter-clockwise.
    Quit,        ///< Ask the application to shut down.
};

struct KeyMapping {
    KeyAction action{KeyAction::None};
    core::ButtonId button{core::ButtonId::Play};
};

/// The keyboard layout, as a pure function.
///
/// Separated from the input thread so the map can be unit tested without a
/// terminal, and so it is one obvious place to change when the bindings need
/// adjusting.  The layout deliberately mirrors the physical panel:
///
///   space          Play              1 2 3 4     Track 1..4
///   s              Stop              Enter       Encoder push
///   r              Record            left/right  Encoder turn
///   i              Import            up/down     Encoder turn
///   e              Export            b / Esc     Back
///   u              Undo              q           Quit
///   w              Save
[[nodiscard]] KeyMapping mapKey(int key) noexcept;

/// Owns raw-mode stdin and the key reader thread.
///
/// One object serves both `ButtonInterface` and `EncoderInterface`, because
/// there is only one stdin to read and two independent readers would fight over
/// it.  The two HAL objects below are thin adapters holding a shared reference.
///
/// Restores the terminal in its destructor, including on an abnormal exit path -
/// leaving a developer's shell in raw mode is a genuinely unpleasant bug.
class KeyboardInputHub {
public:
    KeyboardInputHub();
    ~KeyboardInputHub();

    KeyboardInputHub(const KeyboardInputHub&) = delete;
    KeyboardInputHub& operator=(const KeyboardInputHub&) = delete;

    /// Idempotent; the second caller (encoder or buttons, whichever is second)
    /// is a no-op.
    core::Status start();

    /// Stops the reader thread and restores the terminal.  Idempotent.
    void stop() noexcept;

    void setButtonListener(hal::ButtonListener* listener) noexcept;
    void setEncoderListener(hal::EncoderListener* listener) noexcept;

    [[nodiscard]] std::int32_t consumeDelta() noexcept;

    /// True when stdin is a terminal that can be put into raw mode.
    [[nodiscard]] static bool isAvailable() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// `ButtonInterface` over the keyboard.
///
/// Reports `Pressed` immediately followed by `Released`: a terminal delivers
/// keystrokes, not key-down/key-up, so there is no held state to report and
/// `isPressed` is always false.  Long-press and auto-repeat are therefore not
/// available here - which is worth knowing when testing a UI interaction that
/// depends on them.
class KeyboardButtons final : public hal::ButtonInterface {
public:
    explicit KeyboardButtons(std::shared_ptr<KeyboardInputHub> hub) noexcept;

    void setListener(hal::ButtonListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] bool isPressed(core::ButtonId id) const noexcept override;

private:
    std::shared_ptr<KeyboardInputHub> hub_;
};

/// `EncoderInterface` over the arrow keys.
class KeyboardEncoder final : public hal::EncoderInterface {
public:
    explicit KeyboardEncoder(std::shared_ptr<KeyboardInputHub> hub) noexcept;

    void setListener(hal::EncoderListener* listener) noexcept override;
    core::Status start() override;
    void stop() noexcept override;
    [[nodiscard]] std::int32_t consumeDelta() noexcept override;

private:
    std::shared_ptr<KeyboardInputHub> hub_;
};

}  // namespace aw::platform::desktop
