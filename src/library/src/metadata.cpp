#include "kessoku/library/metadata.h"

#include "kessoku/core/error.h"
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

// TagLib::RIFF::WAV::File::isValid() only reports that the stream opened.
// Require a RIFF/WAVE container with a readable fmt chunk so a file that
// merely starts with the signature (but has no format) fails instead of
// producing empty metadata. This mirrors the fields ParseWavFormat needs.
bool HasUsableWaveFormat(const std::filesystem::path& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char header[12]{};
    file.read(header, sizeof(header));
    if (file.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    while (file.good()) {
        char chunk[8]{};
        file.read(chunk, sizeof(chunk));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(chunk))) {
            return false;
        }

        const auto byteAt = [&chunk](int index) {
            return static_cast<uint32_t>(
                static_cast<unsigned char>(chunk[index]));
        };
        const uint32_t size = byteAt(4) | (byteAt(5) << 8) |
                              (byteAt(6) << 16) | (byteAt(7) << 24);

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            // Match ParseWavFormat's allocation cap: a huge declared fmt
            // chunk must not make TagLib allocate it or seek past the file.
            if (size < 16 || size > 1024 * 1024) {
                return false;
            }
            char fmt[16]{};
            file.read(fmt, sizeof(fmt));
            return file.gcount() ==
                   static_cast<std::streamsize>(sizeof(fmt));
        }

        // RIFF payloads are word-aligned; an odd size has one pad byte.
        const std::streamoff skip =
            static_cast<std::streamoff>(size) +
            static_cast<std::streamoff>(size & 1u);
        if (!file.seekg(skip, std::ios::cur)) {
            return false;
        }
    }
    return false;
}

} // namespace

namespace kessoku::library {

core::Result<TrackMetadata> ReadTrackMetadata(const std::filesystem::path& filePath) {
    // Validate the RIFF/WAVE structure before TagLib parses it. A .wav path
    // with a bogus header must not make the library parse arbitrary chunks,
    // and a signature-only file must not report success with empty metadata.
    if (IsWavFile(filePath) && !HasUsableWaveFormat(filePath)) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError,
            "Could not open or parse file: " +
                core::WideToUtf8(filePath.wstring())
        );
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
        // WAV — use FileRef's internal WAV file directly.
        TagLib::RIFF::WAV::File* wavFile =
            dynamic_cast<TagLib::RIFF::WAV::File*>(ref.file());
        if (!wavFile || !wavFile->isValid() || !HasUsableWaveFormat(filePath)) {
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

} // namespace kessoku::library
