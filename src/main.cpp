#include <array>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <unistd.h>
#include <termios.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace
{
    struct Position
    {
        int x {};
        int y {};

        bool operator==(const Position& other) const
        {
            return x == other.x && y == other.y;
        }
    };

    enum class Direction
    {
        Up,
        Down,
        Left,
        Right
    };

    struct GameConfig
    {
        int boardWidth {32};
        int boardHeight {18};
        int minFrameDelayMs {160};
        int maxFrameDelayMs {70};
        int treatsForMaxSpeed {12};
    };

    std::filesystem::path executableDirectory()
    {
#ifdef __APPLE__
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        {
            throw std::runtime_error("Failed to resolve executable path.");
        }

        return std::filesystem::weakly_canonical(buffer.data()).parent_path();
#else
        std::array<char, 4096> buffer {};
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
        if (length == -1)
        {
            throw std::runtime_error("Failed to resolve executable path.");
        }

        buffer[static_cast<std::size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data()).parent_path();
#endif
    }

    std::string trim(const std::string& value)
    {
        std::size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
        {
            ++start;
        }

        std::size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        {
            --end;
        }

        return value.substr(start, end - start);
    }

    int parseInteger(const std::string& key, const std::string& value)
    {
        std::size_t processedCharacters = 0;
        const int parsed = std::stoi(value, &processedCharacters);
        if (processedCharacters != value.size())
        {
            throw std::runtime_error("Invalid integer value for config key: " + key);
        }

        return parsed;
    }

    GameConfig loadConfig(const std::string& filePath)
    {
        GameConfig config;
        std::ifstream configFile(filePath);
        if (!configFile)
        {
            throw std::runtime_error("Failed to open config file: " + filePath);
        }

        std::string line;
        int lineNumber = 0;
        while (std::getline(configFile, line))
        {
            ++lineNumber;
            const std::string cleanedLine = trim(line);
            if (cleanedLine.empty() || cleanedLine[0] == '#')
            {
                continue;
            }

            const std::size_t separator = cleanedLine.find('=');
            if (separator == std::string::npos)
            {
                throw std::runtime_error("Invalid config line " + std::to_string(lineNumber) + ": expected key=value");
            }

            const std::string key = trim(cleanedLine.substr(0, separator));
            const std::string value = trim(cleanedLine.substr(separator + 1));

            if (key == "board_width")
            {
                config.boardWidth = parseInteger(key, value);
            }
            else if (key == "board_height")
            {
                config.boardHeight = parseInteger(key, value);
            }
            else if (key == "min_frame_delay_ms")
            {
                config.minFrameDelayMs = parseInteger(key, value);
            }
            else if (key == "max_frame_delay_ms")
            {
                config.maxFrameDelayMs = parseInteger(key, value);
            }
            else if (key == "treats_for_max_speed")
            {
                config.treatsForMaxSpeed = parseInteger(key, value);
            }
            else
            {
                throw std::runtime_error("Unknown config key: " + key);
            }
        }

        if (config.boardWidth < 10)
        {
            throw std::runtime_error("board_width must be at least 10.");
        }

        if (config.boardHeight < 8)
        {
            throw std::runtime_error("board_height must be at least 8.");
        }

        if (config.minFrameDelayMs <= 0 || config.maxFrameDelayMs <= 0)
        {
            throw std::runtime_error("Frame delay values must be positive.");
        }

        if (config.maxFrameDelayMs > config.minFrameDelayMs)
        {
            throw std::runtime_error("max_frame_delay_ms must be less than or equal to min_frame_delay_ms.");
        }

        if (config.treatsForMaxSpeed <= 0)
        {
            throw std::runtime_error("treats_for_max_speed must be greater than 0.");
        }

        return config;
    }

    std::string formatDuration(std::chrono::milliseconds duration)
    {
        const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
        const auto hours = totalSeconds / 3600;
        const auto minutes = (totalSeconds % 3600) / 60;
        const auto seconds = totalSeconds % 60;

        std::ostringstream stream;
        stream << std::setfill('0');
        if (hours > 0)
        {
            stream << hours << ':';
        }

        stream << std::setw(2) << minutes << ':'
               << std::setw(2) << seconds;
        return stream.str();
    }

    std::string currentTimestamp()
    {
        const std::time_t now = std::time(nullptr);
        const std::tm localTime = *std::localtime(&now);

        std::ostringstream stream;
        stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
        return stream.str();
    }

    struct GameRecord
    {
        std::string timestamp;
        int score {0};
        long long durationMs {0};
    };

    class GameStats
    {
    public:
        explicit GameStats(const std::filesystem::path& filePath)
            : filePath_(filePath)
        {
            load();
        }

        int highScore() const
        {
            return highScore_;
        }

        std::size_t gameCount() const
        {
            return records_.size();
        }

        bool recordGame(int score, std::chrono::milliseconds duration)
        {
            const bool brokeRecord = score > highScore_;
            if (brokeRecord)
            {
                highScore_ = score;
            }

            records_.push_back(GameRecord {currentTimestamp(), score, duration.count()});
            save();
            return brokeRecord;
        }

    private:
        void load()
        {
            std::ifstream input(filePath_);
            if (!input)
            {
                return;
            }

            std::string line;
            while (std::getline(input, line))
            {
                const std::string cleanedLine = trim(line);
                if (cleanedLine.empty() || cleanedLine[0] == '#')
                {
                    continue;
                }

                if (cleanedLine.rfind("high_score=", 0) == 0)
                {
                    highScore_ = parseInteger("high_score", cleanedLine.substr(std::string("high_score=").size()));
                    continue;
                }

                if (cleanedLine.rfind("games_played=", 0) == 0)
                {
                    continue;
                }

                const std::size_t firstSeparator = cleanedLine.find('|');
                const std::size_t scoreSeparator = cleanedLine.find("|score=", firstSeparator == std::string::npos ? 0 : firstSeparator);
                const std::size_t durationSeparator = cleanedLine.find("|duration_ms=", scoreSeparator == std::string::npos ? 0 : scoreSeparator);
                if (firstSeparator == std::string::npos || scoreSeparator == std::string::npos || durationSeparator == std::string::npos)
                {
                    continue;
                }

                GameRecord record;
                record.timestamp = cleanedLine.substr(0, firstSeparator);
                record.score = parseInteger("score", cleanedLine.substr(scoreSeparator + 7, durationSeparator - (scoreSeparator + 7)));
                record.durationMs = static_cast<long long>(std::stoll(cleanedLine.substr(durationSeparator + 13)));
                records_.push_back(record);
            }

            if (highScore_ == 0 && !records_.empty())
            {
                for (const GameRecord& record : records_)
                {
                    if (record.score > highScore_)
                    {
                        highScore_ = record.score;
                    }
                }
            }
        }

        void save() const
        {
            std::ofstream output(filePath_, std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error("Failed to write stats file: " + filePath_.string());
            }

            output << "high_score=" << highScore_ << '\n';
            output << "games_played=" << records_.size() << '\n';
            output << "# timestamp|score|duration_ms\n";
            for (const GameRecord& record : records_)
            {
                output << record.timestamp
                       << "|score=" << record.score
                       << "|duration_ms=" << record.durationMs
                       << '\n';
            }
        }

        std::filesystem::path filePath_;
        int highScore_ {0};
        std::vector<GameRecord> records_ {};
    };

    class TerminalController
    {
    public:
        TerminalController()
        {
            if (tcgetattr(STDIN_FILENO, &originalState_) == -1)
            {
                throw std::runtime_error("Failed to read terminal state.");
            }

            termios rawState = originalState_;
            rawState.c_lflag &= static_cast<unsigned long>(~(ICANON | ECHO));
            rawState.c_cc[VMIN] = 0;
            rawState.c_cc[VTIME] = 0;

            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &rawState) == -1)
            {
                throw std::runtime_error("Failed to switch terminal to raw mode.");
            }

            std::cout << "\033[?25l";
            std::cout.flush();
        }

        TerminalController(const TerminalController&) = delete;
        TerminalController& operator=(const TerminalController&) = delete;

        ~TerminalController()
        {
            restore();
        }

        char pollInput() const
        {
            char ch = '\0';
            const ssize_t readResult = read(STDIN_FILENO, &ch, 1);
            if (readResult == 1)
            {
                return ch;
            }

            return '\0';
        }

        void restore()
        {
            if (!restored_)
            {
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalState_);
                std::cout << "\033[?25h\033[0m";
                std::cout.flush();
                restored_ = true;
            }
        }

    private:
        termios originalState_ {};
        bool restored_ {false};
    };

    class Cnake
    {
    public:
        Cnake(int boardWidth, int boardHeight)
        {
            const int startX = boardWidth / 2;
            const int startY = boardHeight / 2;
            body_.push_back({startX, startY});
            body_.push_back({startX - 1, startY});
            body_.push_back({startX - 2, startY});
        }

        const std::deque<Position>& body() const
        {
            return body_;
        }

        const Position& head() const
        {
            return body_.front();
        }

        Direction direction() const
        {
            return direction_;
        }

        void setDirection(Direction nextDirection)
        {
            if (isOpposite(nextDirection))
            {
                return;
            }

            direction_ = nextDirection;
        }

        Position nextHeadPosition() const
        {
            Position head = body_.front();

            switch (direction_)
            {
                case Direction::Up:
                    --head.y;
                    break;
                case Direction::Down:
                    ++head.y;
                    break;
                case Direction::Left:
                    --head.x;
                    break;
                case Direction::Right:
                    ++head.x;
                    break;
            }

            return head;
        }

        void advance(bool grow)
        {
            body_.push_front(nextHeadPosition());
            if (!grow)
            {
                body_.pop_back();
            }
        }

        bool occupies(const Position& position) const
        {
            for (const Position& segment : body_)
            {
                if (segment == position)
                {
                    return true;
                }
            }

            return false;
        }

        bool hitsItself() const
        {
            const Position& head = body_.front();
            for (std::size_t index = 1; index < body_.size(); ++index)
            {
                if (body_[index] == head)
                {
                    return true;
                }
            }

            return false;
        }

        std::size_t length() const
        {
            return body_.size();
        }

    private:
        bool isOpposite(Direction nextDirection) const
        {
            return (direction_ == Direction::Up && nextDirection == Direction::Down)
                || (direction_ == Direction::Down && nextDirection == Direction::Up)
                || (direction_ == Direction::Left && nextDirection == Direction::Right)
                || (direction_ == Direction::Right && nextDirection == Direction::Left);
        }

        std::deque<Position> body_ {};
        Direction direction_ {Direction::Right};
    };

    class CnakeGame
    {
    public:
        CnakeGame(const GameConfig& config, GameStats& stats)
            : width_(config.boardWidth),
              height_(config.boardHeight),
              config_(config),
              cnake_(config.boardWidth, config.boardHeight),
              stats_(stats),
              randomEngine_(std::random_device {}())
        {
            spawnApple();
        }

        void run()
        {
            gameStart_ = std::chrono::steady_clock::now();
            render();

            while (running_)
            {
                handleInput();
                update();
                render();
                std::this_thread::sleep_for(currentFrameDelay());
            }

            const auto gameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - gameStart_);
            const bool brokeRecord = stats_.recordGame(score_, gameDuration);
            renderGameOver(gameDuration, brokeRecord);
        }

        static void requestStop(int)
        {
            interrupted_ = true;
        }

    private:
        void handleInput()
        {
            const char input = terminal_.pollInput();
            switch (input)
            {
                case 'w':
                case 'W':
                    cnake_.setDirection(Direction::Up);
                    break;
                case 's':
                case 'S':
                    cnake_.setDirection(Direction::Down);
                    break;
                case 'a':
                case 'A':
                    cnake_.setDirection(Direction::Left);
                    break;
                case 'd':
                case 'D':
                    cnake_.setDirection(Direction::Right);
                    break;
                case 'q':
                case 'Q':
                    quitRequested_ = true;
                    running_ = false;
                    break;
                default:
                    break;
            }
        }

        void update()
        {
            if (!running_)
            {
                return;
            }

            if (interrupted_)
            {
                running_ = false;
                return;
            }

            const Position nextHead = cnake_.nextHeadPosition();
            if (hitsWall(nextHead))
            {
                running_ = false;
                return;
            }

            const bool ateApple = nextHead == apple_;
            cnake_.advance(ateApple);

            if (cnake_.hitsItself())
            {
                running_ = false;
                return;
            }

            if (ateApple)
            {
                ++score_;
                if (cnake_.length() == static_cast<std::size_t>((width_ - 2) * (height_ - 2)))
                {
                    won_ = true;
                    running_ = false;
                    return;
                }

                spawnApple();
            }
        }

        bool hitsWall(const Position& position) const
        {
            return position.x <= 0 || position.x >= width_ - 1
                || position.y <= 0 || position.y >= height_ - 1;
        }

        void spawnApple()
        {
            std::uniform_int_distribution<int> xDistribution(1, width_ - 2);
            std::uniform_int_distribution<int> yDistribution(1, height_ - 2);

            do
            {
                apple_ = {xDistribution(randomEngine_), yDistribution(randomEngine_)};
            }
            while (cnake_.occupies(apple_));
        }

        void render() const
        {
            std::cout << "\033[2J\033[H";
            std::cout
                << "Cnake  |  Score: " << score_
                << "  |  High: " << stats_.highScore()
                << "  |  Delay: " << currentFrameDelay().count() << "ms"
                << "  |  Move: WASD  |  Quit: Q\n";

            for (int y = 0; y < height_; ++y)
            {
                for (int x = 0; x < width_; ++x)
                {
                    const Position current {x, y};

                    if (x == 0 || x == width_ - 1 || y == 0 || y == height_ - 1)
                    {
                        std::cout << "# ";
                    }
                    else if (current == cnake_.head())
                    {
                        std::cout << "@ ";
                    }
                    else if (current == apple_)
                    {
                        std::cout << "X ";
                    }
                    else if (cnake_.occupies(current))
                    {
                        std::cout << "O ";
                    }
                    else
                    {
                        std::cout << "  ";
                    }
                }

                std::cout << '\n';
            }

            std::cout.flush();
        }

        void renderGameOver(std::chrono::milliseconds gameDuration, bool brokeRecord)
        {
            terminal_.restore();

            if (won_)
            {
                std::cout << "\nYou filled the board.";
            }
            else if (quitRequested_)
            {
                std::cout << "\nThanks for playing.";
            }
            else
            {
                std::cout << "\nGame over.";
            }

            std::cout << "\nScore: " << score_
                      << "\nPlayed: " << formatDuration(gameDuration)
                      << "\nHigh score: " << stats_.highScore()
                      << "\nRecord: " << (brokeRecord ? "new record!" : "no new record")
                      << "\nGames logged: " << stats_.gameCount()
                      << '\n';
        }

        std::chrono::milliseconds currentFrameDelay() const
        {
            const int clampedTreats = score_ < config_.treatsForMaxSpeed ? score_ : config_.treatsForMaxSpeed;
            const double progress = static_cast<double>(clampedTreats) / static_cast<double>(config_.treatsForMaxSpeed);
            const double delayRange = static_cast<double>(config_.minFrameDelayMs - config_.maxFrameDelayMs);
            const int delay = static_cast<int>(config_.minFrameDelayMs - (delayRange * progress));
            return std::chrono::milliseconds(delay);
        }

        const int width_;
        const int height_;
        const GameConfig config_;
        Cnake cnake_;
        GameStats& stats_;
        Position apple_ {};
        TerminalController terminal_ {};
        int score_ {0};
        bool running_ {true};
        bool won_ {false};
        bool quitRequested_ {false};
        std::mt19937 randomEngine_;
        std::chrono::steady_clock::time_point gameStart_ {};
        static inline volatile std::sig_atomic_t interrupted_ {0};
    };
}

int main()
{
    try
    {
        std::signal(SIGINT, CnakeGame::requestStop);
        const std::filesystem::path configPath = executableDirectory() / "cnake.conf";
        const std::filesystem::path statsPath = executableDirectory() / "cnake_stats.txt";
        const GameConfig config = loadConfig(configPath.string());
        GameStats stats(statsPath);
        CnakeGame game(config, stats);
        game.run();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
