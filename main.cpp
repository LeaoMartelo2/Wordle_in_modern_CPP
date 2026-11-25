#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <print>
#include <random>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace wordle::engine {

inline constexpr auto word_length = std::size_t{5};
inline constexpr auto dictionary_size = std::size_t{10};
inline constexpr auto max_attempts = std::size_t{6};

inline constexpr auto word_dictionary = std::array<std::string_view, dictionary_size>{{
    "termo",
    "nobre",
    "lagos",
    "sagaz",
    "fazer",
    "poder",
    "digno",
    "muito",
    "vetor",
    "ponte",
}};

enum class platform_kind {
    windows,
    macos,
    linux,
    other,
};

constexpr auto detect_platform() noexcept -> platform_kind {
#if defined(_WIN32)
    return platform_kind::windows;
#elif defined(__APPLE__)
    return platform_kind::macos;
#elif defined(__linux__)
    return platform_kind::linux;
#else
    return platform_kind::other;
#endif
}

template <platform_kind p>
struct platform_traits {
    static constexpr auto platform_name = std::string_view{"POSIX"};
};

template <>
struct platform_traits<platform_kind::windows> {
    static constexpr auto platform_name = std::string_view{"Windows"};
};

template <>
struct platform_traits<platform_kind::macos> {
    static constexpr auto platform_name = std::string_view{"MacOS"};
};

template <>
struct platform_traits<platform_kind::linux> {
    static constexpr auto platform_name = std::string_view{"Linux"};
};

enum class letter_status {
    correct,
    wrong_position,
    absent,
    neutral,
};

class renderable_cell_interface {
  public:
    virtual ~renderable_cell_interface() = default;
    [[nodiscard]] virtual auto render_cell() const -> std::string = 0;
};

class plain_letter_tile final : public renderable_cell_interface {
    char letter_;
    letter_status status_;

    [[nodiscard]] static auto to_upper_ascii(char ch) noexcept -> char {

        const auto normalize = [](auto c) noexcept -> auto {
            const auto l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return (l >= 'a' && l <= 'z') ? static_cast<char>(l - 'a' + 'A') : c;
            /* que bagulho nojento pqp */
        };
        return normalize(ch);
    }

    [[nodiscard]] constexpr auto bracket_color(void) const noexcept -> std::string_view {

        const auto pick = [this]() constexpr noexcept -> std::string_view {
            switch (status_) {
            case letter_status::correct:
                return "\033[1;32m";
            case letter_status::wrong_position:
                return "\033[1;33m";
            case letter_status::absent:
                return "\033[90m";
            case letter_status::neutral:
                return "\033[37m";
            }
            return "\033[0m";
        };
        return pick();
    }

  public:
    constexpr plain_letter_tile(char letter, letter_status status) noexcept : letter_(letter), status_(status) {}

    [[nodiscard]] auto render_cell() const -> std::string override {
        const auto color = bracket_color();

        const auto build = [&](const auto &c, const auto &ch)->std::string {
            return std::format("{}[\033[0m{}\033[0m{}]\033[0m", c, ch, c);
        };
        return build(color, to_upper_ascii(letter_));
    }
};

class raw_terminal_guard final {
#if defined(_WIN32)
    HANDLE h_in_{INVALID_HANDLE_VALUE};
    HANDLE h_out_{INVALID_HANDLE_VALUE};
    DWORD old_in_mode_{0};
    DWORD old_out_mode_{0};
    bool active_{false};
#else
    termios original_{};
    bool active_{false};
#endif

  public:
    raw_terminal_guard() {
        active_ = [&]() -> bool {
            if constexpr (detect_platform() == platform_kind::windows) {
#if defined(_WIN32)
                h_in_ = ::GetStdHandle(STD_INPUT_HANDLE);
                h_out_ = ::GetStdHandle(STD_OUTPUT_HANDLE);
                bool ok = true;
                if (h_in_ != INVALID_HANDLE_VALUE && ::GetConsoleMode(h_in_, &old_in_mode_)) {
                    const auto raw_in = (old_in_mode_ & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
                                        ENABLE_VIRTUAL_TERMINAL_INPUT;
                    ok = (::SetConsoleMode(h_in_, raw_in) != 0);
                } else {
                    ok = false;
                }
                if (h_out_ != INVALID_HANDLE_VALUE && ::GetConsoleMode(h_out_, &old_out_mode_)) {
                    ::SetConsoleMode(h_out_, old_out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                }
                return ok;
#else
                return false;
#endif
            } else {
#if !defined(_WIN32)
                if (::isatty(STDIN_FILENO) != 1 || ::tcgetattr(STDIN_FILENO, &original_) != 0) {
                    return false;
                }
                auto raw = original_;
                raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                return ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
#else
                return false;
#endif
            }
        }();
    }

    ~raw_terminal_guard() {
        const auto restore = [this]() noexcept -> void {
            if (!active_) {
                return;
            }
            if constexpr (detect_platform() == platform_kind::windows) {
#if defined(_WIN32)
                if (h_in_ != INVALID_HANDLE_VALUE) {
                    ::SetConsoleMode(h_in_, old_in_mode_);
                }
                if (h_out_ != INVALID_HANDLE_VALUE) {
                    ::SetConsoleMode(h_out_, old_out_mode_);
                }
#endif
            } else {
#if !defined(_WIN32)
                ::tcsetattr(STDIN_FILENO, TCSANOW, &original_);
#endif
            }
        };
        restore();
    }

    raw_terminal_guard(const raw_terminal_guard &) = delete;
    auto operator=(const raw_terminal_guard &) -> raw_terminal_guard & = delete;
};

[[nodiscard]] inline auto read_single_key() noexcept -> int {
    const auto read_key = []() noexcept -> int {
        if constexpr (detect_platform() == platform_kind::windows) {
#if defined(_WIN32)
            return ::_getch();
            /* :trollface: */
#else
            return -1;
#endif
        } else {
#if !defined(_WIN32)
            using byte_t = unsigned char;
            auto ch = byte_t{};
            const auto n = ::read(STDIN_FILENO, &ch, 1);
            return (n > 0) ? static_cast<int>(ch) : -1;
#else
            return -1;
#endif
        }
    };
    return read_key();
}

[[nodiscard]] inline auto render_live_buffer(std::string_view buffer) -> std::string {
    const auto build = [](auto buf) -> std::string {
        auto out = std::string{};
        const auto append_slot = [&](std::size_t idx) -> void {
            if (idx < buf.size()) {
                const auto tile = plain_letter_tile{buf[idx], letter_status::neutral};
                out += tile.render_cell();
            } else {
                out += "\033[90m[\033[0m·\033[0m\033[90m]\033[0m";
            }
            if (idx + 1 < word_length) {
                out += ' ';
            }
        };
        for (auto i = std::size_t{0}; i < word_length; ++i) {
            append_slot(i);
        }
        return out;
    };
    return build(buffer);
}

[[nodiscard]] inline auto read_live_guess() -> std::string {
    auto guard = raw_terminal_guard{};
    auto buffer = std::string{};
    buffer.reserve(word_length);

    const auto step = [&buffer]() -> int {
        std::print("\r\033[2K> {}", render_live_buffer(buffer));
        std::fflush(stdout);

        const auto code = read_single_key();
        if (code < 0) {
            buffer.clear();
            return -1;
        }
        if (code == '\n' || code == '\r') {
            std::println("");
            return 1;
        }
        if (code == 127 || code == '\b' || code == 8) {
            if (!buffer.empty()) {
                buffer.pop_back();
            }
            return 0;
        }
        const auto handle_char = [&buffer](auto raw_c) -> void {
            const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(raw_c)));
            if (lower >= 'a' && lower <= 'z' && buffer.size() < word_length) {
                buffer.push_back(lower);
            }
        };
        handle_char(code);
        return 0;
    };

    while (true) {
        const auto state = step();
        if (state == 1 || state == -1) {
            return buffer;
        }
    }
}

[[nodiscard]] inline auto is_valid_guess(std::string_view guess) noexcept -> bool {
    const auto check = [](auto sv) noexcept -> bool {
        if (sv.size() != word_length) {
            return false;
        }
        for (const auto ch : sv) {
            if (ch < 'a' || ch > 'z') {
                return false;
            }
        }
        return true;
    };
    return check(guess);
}

inline auto evaluate_guess(std::string_view guess, std::string_view target) -> std::array<std::string, word_length> {
    const auto eval_one = [target](auto g, auto t) -> std::string {
        const auto status = [&]() -> letter_status {
            if (g == t) {
                return letter_status::correct;
            }
            if (target.find(g) != std::string_view::npos) {
                return letter_status::wrong_position;
            }
            return letter_status::absent;
        }();
        const auto tile = plain_letter_tile{g, status};
        return tile.render_cell();
    };

    auto row = std::array<std::string, word_length>{};
    for (auto i = std::size_t{0}; i < word_length; ++i) {
        row[i] = eval_one(guess[i], target[i]);
    }

    const auto print_row = [](const auto &r) -> void {
        std::print("  ");
        for (auto i = std::size_t{0}; i < r.size(); ++i) {
            std::print("{}", r[i]);
            if (i + 1 < r.size()) {
                std::print(" ");
            }
        }
        std::println("");
    };
    print_row(row);

    return row;
}

[[nodiscard]] inline auto pick_target_word() -> std::string_view {
    const auto pick = []() -> std::string_view {
        auto rd = std::random_device{};
        const auto seed = static_cast<std::mt19937_64::result_type>(rd()) ^
                          static_cast<std::mt19937_64::result_type>(
                              std::chrono::steady_clock::now().time_since_epoch().count());
        auto gen = std::mt19937_64{seed};
        auto dist = std::uniform_int_distribution<std::size_t>{0, dictionary_size - 1};
        return word_dictionary[dist(gen)];
        /* eu quero chorar vei */  
    };
    return pick();
}

inline auto run_game() -> void {
    const auto target = pick_target_word();
    auto board = std::array<std::array<std::string, word_length>, max_attempts>{};
    auto attempts_used = std::size_t{0};
    auto won = false;

    constexpr auto current_platform_name = platform_traits<detect_platform()>::platform_name;
    std::println("Wordle [{}] ({} letras, {} tentativas)", current_platform_name, word_length, max_attempts);

    const auto play_turn = [&]() -> int {
        std::println("Tentativa {}/{}", attempts_used + 1, max_attempts);
        const auto guess = read_live_guess();

        if (!is_valid_guess(guess)) {
            std::println("Digite exatamente {} letras.", word_length);
            return 0;
        }

        board[attempts_used] = evaluate_guess(guess, target);
        ++attempts_used;

        if (guess == target) {
            won = true;
            return 1;
        }
        return 0;
    };

    while (attempts_used < max_attempts) {
        if (play_turn() == 1) {
            break;
        }
    }

    std::println("");
    const auto finish = [&]() -> void {
        if (won) {
            std::println("Acertou em {} tentativa(s).", attempts_used);
        } else {
            std::println("Fim das tentativas. Palavra secreta: {}", target);
        }
    };
    finish();
}

} // namespace wordle::engine
  

auto main() -> int {
    const auto run = []() -> int {
        wordle::engine::run_game();
        return 0;
    };
    return run();
}
