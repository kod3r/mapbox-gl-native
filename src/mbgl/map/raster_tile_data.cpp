#include <mbgl/map/raster_tile_data.hpp>
#include <mbgl/map/source.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/response.hpp>
#include <mbgl/util/worker.hpp>
#include <mbgl/util/work_request.hpp>

#include <sstream>

using namespace mbgl;

RasterTileData::RasterTileData(const TileID& id_,
                               TexturePool &texturePool,
                               const SourceInfo &source_)
    : TileData(id_),
      source(source_),
      env(Environment::Get()),
      bucket(texturePool, layout) {
}

RasterTileData::~RasterTileData() {
    cancel();
}

void RasterTileData::request(Worker& worker,
                       float pixelRatio,
                       const std::function<void()>& callback) {
    std::string url = source.tileURL(id, pixelRatio);
    state = State::loading;

    req = env.request({ Resource::Kind::Tile, url }, [url, callback, &worker, this](const Response &res) {
        req = nullptr;

        if (res.status != Response::Successful) {
            std::stringstream message;
            message <<  "Failed to load [" << url << "]: " << res.message;
            error = message.str();
            state = State::obsolete;
            callback();
            return;
        }

        state = State::loaded;
        data = res.data;

        workRequest = worker.send([this] {
            if (getState() != State::loaded) {
                return;
            }

            if (bucket.setImage(data)) {
                state = State::parsed;
            } else {
                state = State::invalid;
            }
        }, callback);
    });
}

bool RasterTileData::reparse(Worker&, std::function<void()>) {
    assert(false);
}

Bucket* RasterTileData::getBucket(StyleLayer const&) {
    return &bucket;
}

void RasterTileData::cancel() {
    if (state != State::obsolete) {
        state = State::obsolete;
    }
    if (req) {
        env.cancelRequest(req);
        req = nullptr;
    }
    workRequest.reset();
}
