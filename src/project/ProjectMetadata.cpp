#include "aw/project/ProjectMetadata.hpp"

#include "aw/storage/AtomicFileWriter.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace aw::project {
namespace {

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

void appendLine(std::string& out, int indent, std::string_view line) {
    out.append(static_cast<std::size_t>(indent) * 2U, ' ');
    out.append(line);
    out.push_back('\n');
}

void appendClip(std::string& out, const Clip& clip, bool trailingComma) {
    appendLine(out, 4, "{");
    appendLine(out, 5, "\"id\": " + std::to_string(clip.id) + ",");
    appendLine(out, 5, "\"sourceFile\": \"" + escapeJson(clip.sourceFile) + "\",");
    appendLine(out, 5, "\"sourceRange\": {");
    appendLine(out, 6, "\"start\": " + std::to_string(clip.sourceRange.start) + ",");
    appendLine(out, 6, "\"length\": " + std::to_string(clip.sourceRange.length));
    appendLine(out, 5, "},");
    appendLine(out, 5, "\"timelinePosition\": " + std::to_string(clip.timelinePosition) + ",");
    appendLine(out, 5, "\"gain\": " + std::to_string(clip.gain) + ",");
    appendLine(out, 5, "\"fadeInFrames\": " + std::to_string(clip.fadeInFrames) + ",");
    appendLine(out, 5, "\"fadeOutFrames\": " + std::to_string(clip.fadeOutFrames));
    appendLine(out, 4, trailingComma ? "}," : "}");
}

void appendTrack(std::string& out, const Track& track, bool trailingComma) {
    appendLine(out, 3, "{");
    appendLine(out, 4, "\"id\": " + std::to_string(track.id) + ",");
    appendLine(out, 4, "\"name\": \"" + escapeJson(track.name) + "\",");
    appendLine(out, 4, "\"gain\": " + std::to_string(track.gain) + ",");
    appendLine(out, 4, "\"pan\": " + std::to_string(track.pan) + ",");
    appendLine(out, 4, "\"muted\": " + std::string{track.muted ? "true" : "false"} + ",");
    appendLine(out, 4, "\"soloed\": " + std::string{track.soloed ? "true" : "false"} + ",");
    appendLine(out, 4,
               "\"recordArmed\": " + std::string{track.recordArmed ? "true" : "false"} + ",");
    appendLine(out, 4, "\"clips\": [");
    for (std::size_t i = 0; i < track.clips.size(); ++i) {
        appendClip(out, track.clips[i], i + 1 < track.clips.size());
    }
    appendLine(out, 4, "]");
    appendLine(out, 3, trailingComma ? "}," : "}");
}

std::string serialiseProject(const Project& project) {
    const Timeline& timeline = project.timeline();
    std::string out;
    appendLine(out, 0, "{");
    appendLine(out, 1, "\"version\": " + std::to_string(kMetadataSchemaVersion) + ",");
    appendLine(out, 1, "\"name\": \"" + escapeJson(project.name()) + "\",");
    appendLine(out, 1, "\"sampleRate\": " + std::to_string(timeline.sampleRate) + ",");
    appendLine(out, 1, "\"loopRange\": {");
    appendLine(out, 2, "\"start\": " + std::to_string(timeline.loopRange.start) + ",");
    appendLine(out, 2, "\"length\": " + std::to_string(timeline.loopRange.length));
    appendLine(out, 1, "},");
    appendLine(out, 1,
               "\"loopEnabled\": " + std::string{timeline.loopEnabled ? "true" : "false"} + ",");
    appendLine(out, 1, "\"tracks\": [");
    for (std::size_t i = 0; i < timeline.tracks.size(); ++i) {
        appendTrack(out, timeline.tracks[i], i + 1 < timeline.tracks.size());
    }
    appendLine(out, 1, "]");
    appendLine(out, 0, "}");
    return out;
}

core::Result<std::vector<std::byte>> readAll(hal::StorageInterface& storage, const core::Path& path) {
    auto file = storage.open(path, hal::OpenMode::Read);
    if (!file) {
        return file.error();
    }
    auto size = file.value()->size();
    if (!size) {
        return size.error();
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size.value()));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto read = file.value()->read(std::span<std::byte>{bytes}.subspan(offset));
        if (!read) {
            return read.error();
        }
        if (read.value() == 0) {
            break;
        }
        offset += read.value();
    }
    bytes.resize(offset);
    return bytes;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_{text} {}

    core::Result<Project> parseProject(const core::Path& projectRoot) {
        if (!consume('{')) {
            return invalid("expected '{'");
        }

        std::uint32_t version = 0;
        std::string name;
        Project project{"", projectRoot};
        bool haveVersion = false;
        bool haveName = false;
        bool haveSampleRate = false;
        bool haveLoopRange = false;
        bool haveLoopEnabled = false;
        bool haveTracks = false;

        while (true) {
            skipWhitespace();
            if (consume('}')) {
                break;
            }

            auto key = parseString();
            if (!key) {
                return key.error();
            }
            if (!consume(':')) {
                return invalid("expected ':'");
            }

            if (key.value() == "version") {
                auto parsed = parseUnsigned();
                if (!parsed) return parsed.error();
                version = parsed.value();
                haveVersion = true;
            } else if (key.value() == "name") {
                auto parsed = parseString();
                if (!parsed) return parsed.error();
                name = std::move(parsed).value();
                haveName = true;
            } else if (key.value() == "sampleRate") {
                auto parsed = parseUnsigned();
                if (!parsed) return parsed.error();
                project.mutableTimeline().sampleRate = parsed.value();
                haveSampleRate = true;
            } else if (key.value() == "loopRange") {
                auto parsed = parseFrameRange();
                if (!parsed) return parsed.error();
                project.mutableTimeline().loopRange = parsed.value();
                haveLoopRange = true;
            } else if (key.value() == "loopEnabled") {
                auto parsed = parseBool();
                if (!parsed) return parsed.error();
                project.mutableTimeline().loopEnabled = parsed.value();
                haveLoopEnabled = true;
            } else if (key.value() == "tracks") {
                auto parsed = parseTracks();
                if (!parsed) return parsed.error();
                project.mutableTimeline().tracks = std::move(parsed).value();
                haveTracks = true;
            } else {
                return invalid("unexpected key");
            }

            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (peek() == '}') {
                continue;
            }
            return invalid("expected ',' or '}'");
        }

        if (!haveVersion || !haveName || !haveSampleRate || !haveLoopRange || !haveLoopEnabled ||
            !haveTracks) {
            return invalid("missing required metadata field");
        }
        if (version > kMetadataSchemaVersion) {
            return core::makeError(core::ErrorCode::UnsupportedFormat, "metadata schema too new");
        }

        project.setName(std::move(name));
        for (core::TrackId i = 0; i < maxTrackId(project.mutableTimeline().tracks); ++i) {
            (void)project.allocateTrackId();
        }
        for (core::ClipId i = 0; i < maxClipId(project.mutableTimeline().tracks); ++i) {
            (void)project.allocateClipId();
        }
        project.markSaved();
        return project;
    }

private:
    char peek() const noexcept { return position_ < text_.size() ? text_[position_] : '\0'; }

    void skipWhitespace() noexcept {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        skipWhitespace();
        if (peek() != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    core::Error invalid(std::string message) const {
        return core::makeError(core::ErrorCode::InvalidFormat, std::move(message));
    }

    core::Result<std::string> parseString() {
        skipWhitespace();
        if (peek() != '"') {
            return invalid("expected string");
        }
        ++position_;
        std::string out;
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (position_ >= text_.size()) return invalid("unterminated escape");
                const char escaped = text_[position_++];
                switch (escaped) {
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: return invalid("unsupported escape");
                }
            } else {
                out.push_back(c);
            }
        }
        return invalid("unterminated string");
    }

    template <typename Integer>
    core::Result<Integer> parseInteger() {
        skipWhitespace();
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
        if (position_ == start || (position_ == start + 1 && text_[start] == '-')) {
            return invalid("expected integer");
        }

        Integer value{};
        const auto* first = text_.data() + static_cast<std::ptrdiff_t>(start);
        const auto* last = text_.data() + static_cast<std::ptrdiff_t>(position_);
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc{}) {
            return invalid("bad integer");
        }
        return value;
    }

    core::Result<std::uint32_t> parseUnsigned() { return parseInteger<std::uint32_t>(); }

    core::Result<float> parseFloat() {
        skipWhitespace();
        const std::size_t start = position_;
        if (peek() == '-') ++position_;
        bool seenDigit = false;
        while (position_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
            seenDigit = true;
        }
        if (peek() == '.') {
            ++position_;
            while (position_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[position_])) != 0) {
                ++position_;
                seenDigit = true;
            }
        }
        if (!seenDigit) return invalid("expected float");
        const std::string token{text_.substr(start, position_ - start)};
        try {
            return std::stof(token);
        } catch (...) {
            return invalid("bad float");
        }
    }

    core::Result<bool> parseBool() {
        skipWhitespace();
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            return true;
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            return false;
        }
        return invalid("expected bool");
    }

    core::Result<core::FrameRange> parseFrameRange() {
        if (!consume('{')) return invalid("expected frame range object");
        core::FrameRange range;
        bool haveStart = false;
        bool haveLength = false;
        while (true) {
            skipWhitespace();
            if (consume('}')) break;
            auto key = parseString();
            if (!key) return key.error();
            if (!consume(':')) return invalid("expected ':'");
            if (key.value() == "start") {
                auto value = parseInteger<core::FrameIndex>();
                if (!value) return value.error();
                range.start = value.value();
                haveStart = true;
            } else if (key.value() == "length") {
                auto value = parseInteger<core::FrameCount>();
                if (!value) return value.error();
                range.length = value.value();
                haveLength = true;
            } else {
                return invalid("unexpected frame range key");
            }
            skipWhitespace();
            if (consume(',')) continue;
            if (peek() == '}') continue;
            return invalid("expected ',' or '}'");
        }
        if (!haveStart || !haveLength) return invalid("incomplete frame range");
        return range;
    }

    core::Result<Clip> parseClip() {
        if (!consume('{')) return invalid("expected clip object");
        Clip clip;
        bool haveId = false;
        bool haveSourceFile = false;
        bool haveSourceRange = false;
        bool haveTimelinePosition = false;
        bool haveGain = false;
        bool haveFadeIn = false;
        bool haveFadeOut = false;

        while (true) {
            skipWhitespace();
            if (consume('}')) break;
            auto key = parseString();
            if (!key) return key.error();
            if (!consume(':')) return invalid("expected ':'");

            if (key.value() == "id") {
                auto value = parseInteger<core::ClipId>();
                if (!value) return value.error();
                clip.id = value.value();
                haveId = true;
            } else if (key.value() == "sourceFile") {
                auto value = parseString();
                if (!value) return value.error();
                clip.sourceFile = std::move(value).value();
                haveSourceFile = true;
            } else if (key.value() == "sourceRange") {
                auto value = parseFrameRange();
                if (!value) return value.error();
                clip.sourceRange = value.value();
                haveSourceRange = true;
            } else if (key.value() == "timelinePosition") {
                auto value = parseInteger<core::FrameIndex>();
                if (!value) return value.error();
                clip.timelinePosition = value.value();
                haveTimelinePosition = true;
            } else if (key.value() == "gain") {
                auto value = parseFloat();
                if (!value) return value.error();
                clip.gain = value.value();
                haveGain = true;
            } else if (key.value() == "fadeInFrames") {
                auto value = parseInteger<core::FrameCount>();
                if (!value) return value.error();
                clip.fadeInFrames = value.value();
                haveFadeIn = true;
            } else if (key.value() == "fadeOutFrames") {
                auto value = parseInteger<core::FrameCount>();
                if (!value) return value.error();
                clip.fadeOutFrames = value.value();
                haveFadeOut = true;
            } else {
                return invalid("unexpected clip key");
            }

            skipWhitespace();
            if (consume(',')) continue;
            if (peek() == '}') continue;
            return invalid("expected ',' or '}'");
        }

        if (!haveId || !haveSourceFile || !haveSourceRange || !haveTimelinePosition || !haveGain ||
            !haveFadeIn || !haveFadeOut) {
            return invalid("incomplete clip");
        }
        return clip;
    }

    core::Result<std::vector<Clip>> parseClips() {
        if (!consume('[')) return invalid("expected clips array");
        std::vector<Clip> clips;
        while (true) {
            skipWhitespace();
            if (consume(']')) break;
            auto clip = parseClip();
            if (!clip) return clip.error();
            clips.push_back(std::move(clip).value());
            skipWhitespace();
            if (consume(',')) continue;
            if (peek() == ']') continue;
            return invalid("expected ',' or ']'");
        }
        return clips;
    }

    core::Result<Track> parseTrack() {
        if (!consume('{')) return invalid("expected track object");
        Track track;
        bool haveId = false;
        bool haveName = false;
        bool haveGain = false;
        bool havePan = false;
        bool haveMuted = false;
        bool haveSoloed = false;
        bool haveRecordArmed = false;
        bool haveClips = false;

        while (true) {
            skipWhitespace();
            if (consume('}')) break;
            auto key = parseString();
            if (!key) return key.error();
            if (!consume(':')) return invalid("expected ':'");

            if (key.value() == "id") {
                auto value = parseInteger<core::TrackId>();
                if (!value) return value.error();
                track.id = value.value();
                haveId = true;
            } else if (key.value() == "name") {
                auto value = parseString();
                if (!value) return value.error();
                track.name = std::move(value).value();
                haveName = true;
            } else if (key.value() == "gain") {
                auto value = parseFloat();
                if (!value) return value.error();
                track.gain = value.value();
                haveGain = true;
            } else if (key.value() == "pan") {
                auto value = parseFloat();
                if (!value) return value.error();
                track.pan = value.value();
                havePan = true;
            } else if (key.value() == "muted") {
                auto value = parseBool();
                if (!value) return value.error();
                track.muted = value.value();
                haveMuted = true;
            } else if (key.value() == "soloed") {
                auto value = parseBool();
                if (!value) return value.error();
                track.soloed = value.value();
                haveSoloed = true;
            } else if (key.value() == "recordArmed") {
                auto value = parseBool();
                if (!value) return value.error();
                track.recordArmed = value.value();
                haveRecordArmed = true;
            } else if (key.value() == "clips") {
                auto value = parseClips();
                if (!value) return value.error();
                track.clips = std::move(value).value();
                haveClips = true;
            } else {
                return invalid("unexpected track key");
            }

            skipWhitespace();
            if (consume(',')) continue;
            if (peek() == '}') continue;
            return invalid("expected ',' or '}'");
        }

        if (!haveId || !haveName || !haveGain || !havePan || !haveMuted || !haveSoloed ||
            !haveRecordArmed || !haveClips) {
            return invalid("incomplete track");
        }
        return track;
    }

    core::Result<std::vector<Track>> parseTracks() {
        if (!consume('[')) return invalid("expected tracks array");
        std::vector<Track> tracks;
        while (true) {
            skipWhitespace();
            if (consume(']')) break;
            auto track = parseTrack();
            if (!track) return track.error();
            tracks.push_back(std::move(track).value());
            skipWhitespace();
            if (consume(',')) continue;
            if (peek() == ']') continue;
            return invalid("expected ',' or ']'");
        }
        return tracks;
    }

    static core::TrackId maxTrackId(const std::vector<Track>& tracks) noexcept {
        core::TrackId max = 0;
        for (const Track& track : tracks) {
            max = std::max(max, track.id);
        }
        return max;
    }

    static core::ClipId maxClipId(const std::vector<Track>& tracks) noexcept {
        core::ClipId max = 0;
        for (const Track& track : tracks) {
            for (const Clip& clip : track.clips) {
                max = std::max(max, clip.id);
            }
        }
        return max;
    }

    std::string_view text_;
    std::size_t position_{0};
};

std::span<const std::byte> asBytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

core::Status MetadataSerializer::save(hal::StorageInterface& storage,
                                      const Project& project) {
    storage::AtomicFileWriter writer{storage, project.paths().metadataFile()};
    if (auto status = writer.open(); !status) {
        return status;
    }

    const std::string serialised = serialiseProject(project);
    if (auto status = writer.write(asBytes(serialised)); !status) {
        return status;
    }
    return writer.commit();
}

core::Result<Project> MetadataSerializer::load(hal::StorageInterface& storage,
                                               const core::Path& projectRoot) {
    const auto bytes = readAll(storage, storage::ProjectPaths{projectRoot}.metadataFile());
    if (!bytes) {
        return bytes.error();
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    Parser parser{text};
    return parser.parseProject(projectRoot);
}

}  // namespace aw::project
