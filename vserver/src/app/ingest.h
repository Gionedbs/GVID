#pragma once
#include <optional>
#include <string>
#include "catalog/catalog.h"
#include "record/mxf_writer.h"
#include "record/recorder.h"
namespace vs {
struct IngestResult {
    bool ok = false;
    int64_t clipId = 0;
    RecordingStats stats;
    std::string error;
};

// Registra una clip e, solo se il file è completo, la inserisce a catalogo.
inline IngestResult ingestClip(ICaptureSource& source, Catalog& catalog, ClipRecord clip, int audioTracks, std::size_t bufferFrames = 25) {
    IngestResult result;
    MxfWriter writer;
    MxfWriter::Options options;
    options.path = clip.path;
    options.audioTracks = audioTracks;
    options.startTimecode = clip.startTimecode;
    if (!writer.open(options)) { result.error = writer.lastError(); return result; }
    ChannelRecorder recorder(source, writer, bufferFrames);
    recorder.start();
    result.stats = recorder.join();
    if (!result.stats.ok) { result.error = result.stats.error; return result; }
    clip.frames = result.stats.written;
    clip.repeatedFrames = result.stats.repeated;
    const std::optional<int64_t> id = catalog.addClip(clip);
    if (!id) { result.error = catalog.lastError(); return result; }
    result.clipId = *id;
    result.ok = true;
    return result;
}
}  // namespace vs
