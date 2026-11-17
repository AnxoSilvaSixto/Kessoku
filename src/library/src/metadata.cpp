#include "kessoku/library/metadata.h"

#include "kessoku/audio/wav_format.h"
#include "kessoku/core/error.h"
#include "kessoku/core/library_root.h"
#include "kessoku/core/utf8.h"

#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/wavfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>

#include <cerrno>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>

namespace {

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

// libFLAC accepts the fLaC stream marker either at offset 0 or immediately
// after an ID3v2 tag, and rejects anything else. TagLib's FLAC detection is
// looser (it scans for the marker), so a .flac that TagLib can open may still
// fail at play time. Mirror libFLAC's opening rule here so the library list
// only contains files the player can actually decode.
bool HasFlacStreamMarker(const std::filesystem::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char magic[4]{};
    file.read(magic, sizeof(magic));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(magic))) {
        return false;
    }
    if (std::memcmp(magic, "fLaC", 4) == 0) {
        return true;
    }
    if (std::memcmp(magic, "ID3", 3) != 0) {
        return false;
    }

    // ID3v2 header: "ID3", major/minor version, flags, synchsafe size.
    char header[10]{};
    file.clear();
    file.seekg(0, std::ios::beg);
    file.read(header, sizeof(header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return false;
    }
    for (int i = 6; i < 10; ++i) {
        if ((static_cast<unsigned char>(header[i]) & 0x80u) != 0) {
            return false; // not a valid synchsafe tag length
        }
    }
    const uint32_t tagSize =
        (static_cast<uint32_t>(static_cast<unsigned char>(header[6])) << 21) |
        (static_cast<uint32_t>(static_cast<unsigned char>(header[7])) << 14) |
        (static_cast<uint32_t>(static_cast<unsigned char>(header[8])) << 7) |
        static_cast<uint32_t>(static_cast<unsigned char>(header[9]));
    // ID3v2.4 allows a 10-byte footer when flag 0x10 is set.
    const uint64_t skip =
        10ull + tagSize +
        ((static_cast<unsigned char>(header[5]) & 0x10u) != 0 ? 10ull : 0ull);
    file.clear();
    file.seekg(static_cast<std::streamoff>(skip), std::ios::beg);
    file.read(magic, sizeof(magic));
    return file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::memcmp(magic, "fLaC", 4) == 0;
}

// The player's own WAV validator, not a shallower copy. A .wav may only be
// listed when it has the RIFF/WAVE signature, PCM fmt fields, consistent
// block alignment, a data chunk whose declared range fits inside the file,
// and a bit depth/channel count with an exact exclusive-mode container.
// Anything less would list a track that fails at the first click.
bool IsPlayableWav(const std::filesystem::path& filePath) {
    kessoku::audio::WavFileInfo info{};
    std::string error;
    return kessoku::audio::ParseWavFile(filePath.wstring(), info, error) ==
           kessoku::audio::WavValidation::Ok;
}

} // namespace

namespace kessoku::library {

namespace {

core::Result<TrackMetadata> ReadTrackMetadataImpl(
    const std::filesystem::path& filePath,
    const core::LibraryRoot* root) {
    // Check-then-open cannot be fully closed without binding the canonical
    // handle to the parser. Re-resolving here narrows the window at the
    // point that matters: the caller can no longer rely on a containment
    // result from an earlier scan.
    const auto stillContained = [&filePath, root] {
        return root == nullptr || root->Contains(filePath.wstring());
    };

    if (!stillContained()) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::InvalidPath,
            "Metadata path resolves outside the library root");
    }

    // Validate the container before TagLib parses it. A .wav path with a
    // bogus header must not make the library parse arbitrary chunks, a
    // signature-only file must not report success with empty metadata, and a
    // .flac without the stream marker must not list a track libFLAC refuses.
    if (IsWavFile(filePath) && !IsPlayableWav(filePath)) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError,
            "Could not open or parse file: " +
                core::WideToUtf8(filePath.wstring())
        );
    }
    if (IsFlacFile(filePath) && !HasFlacStreamMarker(filePath)) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError,
            "Could not open or parse file: " +
                core::WideToUtf8(filePath.wstring())
        );
    }

    // Re-check immediately before TagLib opens the path. The validator above
    // also opened it, so a swap between the two opens is refused here.
    if (!stillContained()) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::InvalidPath,
            "Metadata path resolves outside the library root");
    }

    TagLib::FileRef ref(
        filePath.wstring().c_str(),
        false // don't read audio properties — we only need tags
    );

    if (ref.isNull()) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError,
            "Could not open or parse file: " +
                core::WideToUtf8(filePath.wstring())
        );
    }

    TrackMetadata meta{};

    if (IsFlacFile(filePath)) {
        // FileRef selects a parser by content, not by extension. Require the
        // opened file to actually be FLAC so a WAV renamed to .flac cannot be
        // reported as a successful FLAC metadata read.
        TagLib::FLAC::File* flacFile =
            dynamic_cast<TagLib::FLAC::File*>(ref.file());
        if (!flacFile || !flacFile->isValid()) {
            return core::Result<TrackMetadata>::Err(
                core::ErrorCode::UnknownError,
                "Could not open or parse file: " +
                    core::WideToUtf8(filePath.wstring())
            );
        }

        TagLib::Tag* tag = flacFile->tag();
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
        // WAV — use FileRef's internal WAV file directly. The container and
        // playable-format checks already ran in IsPlayableWav(), so this is
        // only the parser type guard.
        TagLib::RIFF::WAV::File* wavFile =
            dynamic_cast<TagLib::RIFF::WAV::File*>(ref.file());
        if (!wavFile || !wavFile->isValid()) {
            return core::Result<TrackMetadata>::Err(
                core::ErrorCode::UnknownError,
                "Could not open or parse file: " +
                    core::WideToUtf8(filePath.wstring())
            );
        }

        if (wavFile->hasID3v2Tag()) {
            TagLib::ID3v2::Tag* id3v2 = wavFile->ID3v2Tag();
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
    // the LibraryRoot overload below so containment is re-checked at open
    // time; try to do the same for new callers.
    return ReadTrackMetadataImpl(filePath, nullptr);
}

core::Result<TrackMetadata> ReadTrackMetadata(
    const core::LibraryRoot& root,
    const std::filesystem::path& filePath) {
    return ReadTrackMetadataImpl(filePath, &root);
}

} // namespace kessoku::library
