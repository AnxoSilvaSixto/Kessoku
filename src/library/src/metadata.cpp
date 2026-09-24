#include "kessoku/library/metadata.h"

#include "kessoku/core/error.h"

#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/wavfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>

#include <cwctype>

namespace {

std::wstring TagLibStringToWString(const TagLib::String& s) {
    return s.toWString();
}

uint32_t ParseTrackNumber(const TagLib::String& s) {
    bool ok = false;
    long long val = s.toLongLong(&ok, 10);
    if (!ok || val <= 0 || val > 0xFFFFFFFF) {
        return 0;
    }
    return static_cast<uint32_t>(val);
}

bool IsFlacFile(const std::filesystem::path& filePath) {
    std::wstring ext = filePath.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".flac";
}

} // namespace

namespace kessoku::library {

core::Result<TrackMetadata> ReadTrackMetadata(const std::filesystem::path& filePath) {
    TagLib::FileRef ref(
        filePath.wstring().c_str(),
        false // don't read audio properties — we only need tags
    );

    if (ref.isNull()) {
        return core::Result<TrackMetadata>::Err(
            core::ErrorCode::UnknownError,
            "Could not open or parse file: " + filePath.string()
        );
    }

    TrackMetadata meta{};

    // FLAC stores tags in VorbisComment, accessible via basic Tag API
    // WAV with ID3v2 uses property map
    if (IsFlacFile(filePath)) {
        TagLib::Tag* tag = ref.tag();
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
            if (tag->track() > 0) {
                meta.trackNumber = static_cast<uint32_t>(tag->track());
            }
        }
    } else {
        // WAV (ID3v2) — use property map
        TagLib::PropertyMap props = ref.properties();

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

    return core::Result<TrackMetadata>::Ok(std::move(meta));
}

} // namespace kessoku::library
