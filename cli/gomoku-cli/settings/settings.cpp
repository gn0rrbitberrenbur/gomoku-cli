#include "settings/settings.hpp"

#include "file_handling/file_io.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cli
{
namespace
{
/** Checks if a string contains any control characters.
 * @param text The string to check.
 * @return True if the string contains control characters, false otherwise.
 */
bool hasControl(const std::string& text)
{
    return std::any_of(text.begin(), text.end(),
                       [](unsigned char ch) { return ch < 32 || ch == 127; });
}
} // namespace

bool validSha256(const std::string& hash);
std::string sha256(const std::filesystem::path& path);

/**
 * Constructs an EngineStore object by loading engine entries from the specified file.
 * @param file The path to the engine registry file.
 * @throws std::runtime_error if the file cannot be read or contains invalid entries.
 */
EngineStore::EngineStore(std::filesystem::path file) : file_(std::move(file))
{
    if (!std::filesystem::exists(file_))
    {
        return;
    }
    std::ifstream input(file_);
    std::string line;
    if (!std::getline(input, line))
    {
        throw std::runtime_error("Invalid engine registry: " + file_.string());
    }
    const bool version1 = line == "MOKU_ENGINES 1";
    const bool version2 = line == "MOKU_ENGINES 2";
    if (!version1 && !version2)
    {
        throw std::runtime_error("Invalid engine registry: " + file_.string());
    }

    while (std::getline(input, line))
    {
        std::istringstream row(line);
        std::string name;
        std::string path;
        std::string hash;
        if (!(row >> std::quoted(name) >> std::quoted(path)))
        {
            throw std::runtime_error("Malformed entry in engine registry: " + file_.string());
        }
        if (version2)
        {
            if (!(row >> hash))
            {
                throw std::runtime_error("Malformed entry in engine registry: " + file_.string());
            }
        }
        row >> std::ws;
        if (!row.eof() || name.empty() || path.empty() || hasControl(name) || hasControl(path))
        {
            throw std::runtime_error("Invalid entry in engine registry: " + file_.string());
        }

        EngineEntry entry{name, std::filesystem::u8path(path), hash};
        if (version1 && std::filesystem::is_regular_file(entry.executable))
        {
            entry.sha256 = calculateSha256(entry.executable);
        }
        else if (version2 && !validSha256(entry.sha256))
        {
            throw std::runtime_error("Invalid SHA-256 entry in engine registry: " + file_.string());
        }
        validateUnique(entry, entries_.size());
        entries_.push_back(std::move(entry));
    }
    if (input.bad())
    {
        throw std::runtime_error("Cannot read engine registry: " + file_.string());
    }
    if (version1 && std::all_of(entries_.begin(), entries_.end(),
                                [](const EngineEntry& entry) { return validSha256(entry.sha256); }))
    {
        save(entries_);
    }
}

/**
 * Returns a constant reference to the vector of engine entries.
 * @return A constant reference to the vector of engine entries.
 */
const std::vector<EngineEntry>& EngineStore::entries() const
{
    return entries_;
}

/**
 * Returns a constant reference to the path of the engine registry file.
 * @return A constant reference to the path of the engine registry file.
 */
const std::filesystem::path& EngineStore::file() const
{
    return file_;
}

/**
 * Calculates the SHA-256 hash of the binary at the given path.
 * @param executable The path to the executable file.
 * @return The SHA-256 hash of the file as a hexadecimal string.
 */
std::string EngineStore::calculateSha256(const std::filesystem::path& executable)
{
    return sha256(executable);
}

/**
 * Validates an engine entry.
 * @param entry The engine entry to validate.
 * @throws std::invalid_argument if the entry is invalid or its binary hash changed.
 */
void EngineStore::validate(const EngineEntry& entry)
{
    if (entry.name.empty() || entry.name.find_first_not_of(' ') == std::string::npos ||
        hasControl(entry.name) || entry.name.size() > 80)
    {
        throw std::invalid_argument("Use an engine name of 1-80 characters without control codes.");
    }
    if (entry.executable.empty() || hasControl(entry.executable.u8string()) ||
        !std::filesystem::is_regular_file(entry.executable))
    {
        throw std::invalid_argument("The engine path must point to an existing executable file.");
    }
#ifndef _WIN32
    if (access(entry.executable.c_str(), X_OK) != 0)
    {
        throw std::invalid_argument("The engine file does not have execute permission.");
    }
#endif
    if (!entry.sha256.empty())
    {
        if (!validSha256(entry.sha256))
        {
            throw std::invalid_argument("The registered engine has an invalid SHA-256 hash.");
        }
        const std::string actual = calculateSha256(entry.executable);
        if (actual != entry.sha256)
        {
            throw std::invalid_argument("Security check failed: the engine binary changed since "
                                        "registration (SHA-256 mismatch).");
        }
    }
}

/**
 * Validates that an engine entry has a unique name in the store, ignoring a specific index.
 * @param entry The engine entry to validate.
 * @param ignored The index of the entry to ignore during validation.
 * @throws std::invalid_argument if an entry with the same name already exists.
 */
void EngineStore::validateUnique(const EngineEntry& entry, std::size_t ignored) const
{
    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        if (i != ignored && entries_[i].name == entry.name)
        {
            throw std::invalid_argument("An engine with that name already exists.");
        }
    }
}

/**
 * Adds a new engine entry to the store and saves the updated list to the file.
 * @param entry The engine entry to add.
 * @throws std::invalid_argument if the entry is invalid.
 * @throws std::runtime_error if the file cannot be written.
 */
void EngineStore::add(EngineEntry entry)
{
    validate(entry);
    validateUnique(entry, entries_.size());
    entry.executable = std::filesystem::absolute(entry.executable).lexically_normal();
    entry.sha256 = calculateSha256(entry.executable);
    auto next = entries_;
    next.push_back(std::move(entry));
    save(next);
    entries_ = std::move(next);
}

/**
 * Edits an existing engine entry in the store at the specified index and saves the updated list to
 * the file.
 * @param index The index of the engine entry to edit.
 * @param entry The new engine entry to replace the existing one.
 * @throws std::out_of_range if the index is invalid.
 */
void EngineStore::edit(std::size_t index, EngineEntry entry)
{
    if (index >= entries_.size())
    {
        throw std::out_of_range("Unknown engine.");
    }
    validate(entry);
    validateUnique(entry, index);
    entry.executable = std::filesystem::absolute(entry.executable).lexically_normal();
    entry.sha256 = calculateSha256(entry.executable);
    auto next = entries_;
    next[index] = std::move(entry);
    save(next);
    entries_ = std::move(next);
}

/**
 * Removes an engine entry from the store at the specified index and saves the updated list to the
 * file.
 * @param index The index of the engine entry to remove.
 * @throws std::out_of_range if the index is invalid.
 */
void EngineStore::remove(std::size_t index)
{
    if (index >= entries_.size())
    {
        throw std::out_of_range("Unknown engine.");
    }
    auto next = entries_;
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    save(next);
    entries_ = std::move(next);
}

/**
 * Saves the current list of engine entries to the engine registry file atomically.
 * @param entries The vector of engine entries to save.
 */
void EngineStore::save(const std::vector<EngineEntry>& entries) const
{
    std::ostringstream output;
    output << "MOKU_ENGINES 2\n";
    for (const auto& entry : entries)
    {
        output << std::quoted(entry.name) << ' ' << std::quoted(entry.executable.u8string()) << ' '
               << entry.sha256 << '\n';
    }
    writeFileAtomically(file_, output.str(), "engine registry");
}

std::filesystem::path defaultSettingsPath()
{
#ifdef _WIN32
    if (const wchar_t* directory = _wgetenv(L"APPDATA"))
    {
        if (*directory)
        {
            return std::filesystem::path(directory) / "moku-cli" / "engines.cfg";
        }
    }
#else
    if (const char* directory = std::getenv("XDG_CONFIG_HOME"))
    {
        if (*directory)
        {
            return std::filesystem::u8path(directory) / "moku-cli" / "engines.cfg";
        }
    }
    if (const char* directory = std::getenv("HOME"))
    {
        return std::filesystem::u8path(directory) / ".config" / "moku-cli" / "engines.cfg";
    }
#endif
    throw std::runtime_error("No configuration directory found. Use --config <path>.");
}

/**
 * Rotates a 32-bit integer value right by a specified number of bits.
 * @param value The value to rotate.
 * @param bits The number of bits to rotate.
 * @return The rotated value.
 */
uint32_t rotateRight(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

/**
 * Validates the format of a SHA-256 hash.
 * @param hash The hash string to validate.
 * @return True if the hash has a valid format, false otherwise.
 */
bool validSha256(const std::string& hash)
{
    if (hash.size() != 64)
    {
        return false;
    }
    return std::all_of(hash.begin(), hash.end(), [](unsigned char ch)
                       { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

/**
 * Computes the SHA-256 hash of a file.
 * @param path The path to the file to hash.
 * @return The SHA-256 hash of the file as a hexadecimal string.
 * @throws std::runtime_error if the file cannot be read.
 */
std::string sha256(const std::filesystem::path& path)
{
    static constexpr std::array<uint32_t, 64> constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U};

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("Cannot read engine file for SHA-256: " + path.string());
    }

    std::array<uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> block{};
    std::uint64_t total = 0;

    for (;;)
    {
        input.read(reinterpret_cast<char*>(block.data()),
                   static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        if (count == 0)
        {
            if (total % 64 == 0)
            {
                block.fill(0);
                block[0] = 0x80;
                const std::uint64_t bits = total * 8;
                for (int i = 7; i >= 0; --i)
                {
                    block[56 + i] = static_cast<unsigned char>(bits >> ((7 - i) * 8));
                }

                std::array<uint32_t, 64> words{};
                for (std::size_t i = 0; i < 16; ++i)
                {
                    words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                               static_cast<uint32_t>(block[i * 4 + 3]);
                }
                for (std::size_t i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                        rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
                    const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                        rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }
                uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (std::size_t i = 0; i < 64; ++i)
                {
                    const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
                    const uint32_t choice = (e & f) ^ (~e & g);
                    const uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
                    const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
                    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t temp2 = s0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temp1;
                    d = c;
                    c = b;
                    b = a;
                    a = temp1 + temp2;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                state[5] += f;
                state[6] += g;
                state[7] += h;
            }
            break;
        }
        total += static_cast<std::uint64_t>(count);

        if (count != static_cast<std::streamsize>(block.size()))
        {
            const auto start = static_cast<std::size_t>(count);
            block[start] = 0x80;
            std::fill(block.begin() + static_cast<std::ptrdiff_t>(start + 1), block.end(), 0);

            if (count < 56)
            {
                const std::uint64_t bits = total * 8;
                for (int i = 7; i >= 0; --i)
                {
                    block[56 + i] = static_cast<unsigned char>(bits >> ((7 - i) * 8));
                }
                // process 64-byte block below
            }
            else
            {
                // process padded block below, then append a second 64-byte block
                std::array<uint32_t, 64> words{};
                for (std::size_t i = 0; i < 16; ++i)
                {
                    words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                               static_cast<uint32_t>(block[i * 4 + 3]);
                }
                for (std::size_t i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                        rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
                    const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                        rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }
                uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (std::size_t i = 0; i < 64; ++i)
                {
                    const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
                    const uint32_t choice = (e & f) ^ (~e & g);
                    const uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
                    const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
                    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t temp2 = s0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + temp1;
                    d = c;
                    c = b;
                    b = a;
                    a = temp1 + temp2;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                state[5] += f;
                state[6] += g;
                state[7] += h;

                block.fill(0);
                const std::uint64_t bits = total * 8;
                for (int i = 7; i >= 0; --i)
                {
                    block[56 + i] = static_cast<unsigned char>(bits >> ((7 - i) * 8));
                }
            }
        }

        // process this block (unless it was only the first of two padding blocks)
        std::array<uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i)
        {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^
                                (words[i - 15] >> 3);
            const uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^
                                (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + s1 + choice + constants[i] + words[i];
            const uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;

        if (count != static_cast<std::streamsize>(block.size()))
        {
            break;
        }
        block.fill(0);
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint32_t word : state)
    {
        output << std::setw(8) << word;
    }
    return output.str();
}

} // namespace cli
