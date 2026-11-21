#include "kessoku/library/metadata.h"

#include "kessoku/audio/flac_format.h"
#include "kessoku/audio/wav_format.h"
#include "kessoku/core/error.h"
#include "kessoku/core/file_handle.h"
#include "kessoku/core/library_root.h"
#include "kessoku/core/utf8.h"

#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/tiostream.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/wavfile.h>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

// TagLib stream backed by the already-opened, already-validated file handle.
// TagLib never takes ownership of an IOStream; the caller keeps this object
// alive for the whole file-object lifetime. All reads use the same handle the
// containment decision was made on, so a path swap after the check cannot
// redirect metadata parsing to another file.
class HandleStream final : public TagLib::IOStream {
public:
    HandleStream(void* handle, std::wstring name)
        : handle_(static_cast<HANDLE>(handle)), name_(std::move(name)) {
        seek(0, Beginning);
    }

    TagLib::FileName name() const override {
        return TagLib::FileName(name_.c_str());
    }

    TagLib::ByteVector readBlock(size_t length) override {
        if (handle_ == nullptr || length == 0) {
            return {};
        }

        std::vector<char> buffer(length);
        size_t total = 0;
        while (total < length) {
            const DWORD request = static_cast<DWORD>(
                std::min<size_t>(length - total, MAXDWORD));
            DWORD read = 0;
            if (!ReadFile(handle_, buffer.data() + total, request, &read,
                          nullptr) ||
                read == 0) {
                break;
            }
            total += read;
        }
        return TagLib::ByteVector(buffer.data(),
                                  static_cast<unsigned int>(total));
    }

    void writeBlock(const TagLib::ByteVector& /*data*/) override {}
    void insert(const TagLib::ByteVector& /*data*/, TagLib::offset_t /*start*/,
                size_t /*replace*/) override {}
    void removeBlock(TagLib::offset_t /*start*/, size_t /*length*/) override {}

    // Metadata reads never write; TagLib uses this only to decide whether a
    // save is possible, which this application never attempts.
    bool readOnly() const override { return true; }

    bool isOpen() const override { return handle_ != nullptr; }

    void seek(TagLib::offset_t offset, Position p) override {
        if (handle_ == nullptr) {
            return;
        }

        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        DWORD method = FILE_BEGIN;
        if (p == Current) {
            method = FILE_CURRENT;
        } else if (p == End) {
            method = FILE_END;
        }
        SetFilePointerEx(handle_, distance, nullptr, method);
    }

    TagLib::offset_t tell() const override {
        if (handle_ == nullptr) {
            return -1;
        }

        LARGE_INTEGER zero{};
        LARGE_INTEGER position{};
        if (!SetFilePointerEx(handle_, zero, &position, FILE_CURRENT)) {
            return -1;
        }
        return static_cast<TagLib::offset_t>(position.QuadPart);
    }

    TagLib::offset_t length() override {
        if (handle_ == nullptr) {
            return 0;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
            return 0;
        }
        return static_cast<TagLib::offset_t>(size.QuadPart);
    }

    void truncate(TagLib::offset_t /*length*/) override {}

private:
    HANDLE handle_ = nullptr;
    std::wstring name_;
};

std::wstring TagLibStringToWString(const TagLib::String& s) {
    return s.toWString();
}

uint32_t ParseTrackNumber(const TagLib::String& s) {
    // Track tags in the wild can carry a leading/trailing space or an
    // ID3v2-style "number/total" suffix. Accept those forms, but reject
    // anything else that is not a plain in-range unsigned number.
    std::wstring text = s.toWString();
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return 0;
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    text = text.substr(first, last - first + 1);

    const size_t slash = text.find(L'/');
    if (slash != std::wstring::npos) {
        text = text.substr(0, slash);
        const size_t end = text.find_last_not_of(L" \t\r\n");
        if (end == std::wstring::npos) {
            return 0;
        }
        text.resize(end + 1);
    }

    if (text.empty() ||
        text.find_first_not_of(L"0123456789") != std::wstring::npos) {
        return 0;
    }

    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != L'\0' ||
        value == 0 || value > 0xFFFFFFFFULL) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

bool IsFlacFile(const std::filesystem::path& filePath) {
    std::wstring ext = filePath.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".flac";
}

bool IsWavFile(const std::filesystem::path& filePath) {
    std::wstring ext = filePath.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".wav";
}

// Mirror the player's FLAC acceptance gates. ParseFlacFormat already rejects
// a missing marker (unless it follows an ID3v2 tag), a bad STREAMINFO, and
// zero or out-of-range fields; these are the same container checks
// Player::Create applies so a listed track cannot fail at the first click.
bool IsPlayableFlac(void* handle) {
    kessoku::audio::FlacFormat format{};
    if (!kessoku::audio::ParseFlacFormat(handle, format).empty()) {
        return false;
    }
    if (format.bitsPerSample != 8 && format.bitsPerSample != 16 &&
        format.bitsPerSample != 24 && format.bitsPerSample != 32) {
        return false;
    }
    if (format.channelCount == 0 || format.channelCount > 8) {
        return false;
    }
    if (format.totalSamples == 0 || format.totalSamples > UINT32_MAX) {
        return false;
    }
    return true;
}

// The player's own WAV validator, not a shallower copy. A .wav may only be
// listed when it has the RIFF/WAVE signature, PCM fmt fields, consistent
// block alignment, a data chunk whose declared range fits inside the file,
// and a bit depth/channel count with an exact exclusive-mode container.
bool IsPlayableWav(void* handle) {
    kessoku::audio::WavFileInfo info{};
    std::string error;
    return kessoku::audio::ParseWavFile(handle, info, error) ==
           kessoku::audio::WavValidation::Ok;
}

std::string UnreadableMessage(const std::filesystem::path& filePath) {
    return "Could not open or parse file: " +
           kessoku::core::WideToUtf8(filePath.wstring());
}

} // namespace

namespace kessoku::library {

namespace {

core::Result<TrackMetadata> ReadTrackMetadataImpl(
    const std::filesystem::path& filePath,
    const core::LibraryRoot* root) {
    // One open, one containment decision, and every later read from that
    // same handle. When a root is supplied, OpenContained resolves the final
    // path from the handle it just opened; a junction or symlink swapped in
    // afterwards cannot redirect the parse.
    core::Result<core::FileHandle> opened =
        root != nullptr
            ? core::FileHandle::OpenContained(*root, filePath.wstring())
            : core::FileHandle::Open(filePath.wstring());
    if (opened.IsErr()) {
        const core::Error& error = opened.GetError();
        if (error.code == core::ErrorCode::InvalidPath) {
            return core::Result<TrackMetadata>::Err(error.code,
                                                    error.message);
        }
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError, UnreadableMessage(filePath));
    }
    core::FileHandle source = std::move(opened.Value());

    // Validate the container before TagLib parses it. A .wav path with a
    // bogus header must not make the library parse arbitrary chunks, a
    // signature-only file must not report success with empty metadata, and a
    // .flac whose STREAMINFO is unreadable or not playable must not list a
    // track Player::Create would refuse.
    if (IsWavFile(filePath) && !IsPlayableWav(source.get())) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError, UnreadableMessage(filePath));
    }
    if (IsFlacFile(filePath) && !IsPlayableFlac(source.get())) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError, UnreadableMessage(filePath));
    }

    // TagLib reads through this stream, which seeks back to zero in its
    // constructor. The file objects below do not own the stream, so it must
    // outlive them: declaration order puts the stream first.
    HandleStream stream(source.get(), filePath.wstring());

    TrackMetadata meta{};

    if (IsFlacFile(filePath)) {
        TagLib::FLAC::File flacFile(
            &stream,
            false // don't read audio properties - we only need tags
        );
        if (!flacFile.isValid()) {
            return core::Result<TrackMetadata>::Err(
                core::ErrorCode::UnknownError, UnreadableMessage(filePath));
        }

        TagLib::Tag* tag = flacFile.tag();
        if (tag) {
            if (!tag->title().isEmpty()) {
                meta.title = TagLibStringToWString(tag->title());
            }
            if (!tag->artist().isEmpty()) {
                meta.artist = TagLibStringToWString(tag->artist());
            }
            if (!tag->album().isEmpty()) {
                meta.album = TagLibStringToWString(tag->album());
            }

            // Tag::track() clamps or wraps malformed values (a negative tag
            // becomes 4294967293, an over-range one becomes INT_MAX). Parse
            // the raw property instead so out-of-range values become 0.
            TagLib::PropertyMap props = tag->properties();
            auto it = props.find("TRACKNUMBER");
            if (it != props.end() && !it->second.isEmpty()) {
                meta.trackNumber = ParseTrackNumber(it->second.front());
            }
        }
    } else {
        // WAV - the container and playable-format checks already ran above,
        // so this is only the parser type guard.
        TagLib::RIFF::WAV::File wavFile(
            &stream,
            false // don't read audio properties - we only need tags
        );
        if (!wavFile.isValid()) {
            return core::Result<TrackMetadata>::Err(
                core::ErrorCode::UnknownError, UnreadableMessage(filePath));
        }

        if (wavFile.hasID3v2Tag()) {
            TagLib::ID3v2::Tag* id3v2 = wavFile.ID3v2Tag();
            if (id3v2) {
                TagLib::PropertyMap props = id3v2->properties();

                auto it = props.find("TITLE");
                if (it != props.end() && !it->second.isEmpty()) {
                    meta.title = TagLibStringToWString(it->second.front());
                }

                it = props.find("ARTIST");
                if (it != props.end() && !it->second.isEmpty()) {
                    meta.artist = TagLibStringToWString(it->second.front());
                }

                it = props.find("ALBUM");
                if (it != props.end() && !it->second.isEmpty()) {
                    meta.album = TagLibStringToWString(it->second.front());
                }

                it = props.find("TRACKNUMBER");
                if (it != props.end() && !it->second.isEmpty()) {
                    meta.trackNumber = ParseTrackNumber(it->second.front());
                }
            }
        }
        // If no ID3v2 tag: meta stays at defaults (all fields empty/0).
        // We never read from InfoTag().
    }

    return core::Result<TrackMetadata>::Ok(std::move(meta));
}

} // namespace

core::Result<TrackMetadata> ReadTrackMetadata(
    const std::filesystem::path& filePath) {
    // Path-only overload for callers that already established containment
    // (tests and benchmarks reading a known temp file). The application uses
    // the LibraryRoot overload below so containment is checked on the same
    // handle that is parsed; try to do the same for new callers.
    return ReadTrackMetadataImpl(filePath, nullptr);
}

core::Result<TrackMetadata> ReadTrackMetadata(
    const core::LibraryRoot& root,
    const std::filesystem::path& filePath) {
    return ReadTrackMetadataImpl(filePath, &root);
}

} // namespace kessoku::library
