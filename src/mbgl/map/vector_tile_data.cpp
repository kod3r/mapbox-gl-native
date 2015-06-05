#include <mbgl/map/vector_tile_data.hpp>
#include <mbgl/style/style_layer.hpp>
#include <mbgl/style/style_bucket.hpp>
#include <mbgl/map/source.hpp>
#include <mbgl/map/vector_tile.hpp>
#include <mbgl/text/collision_tile.hpp>
#include <mbgl/util/pbf.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/response.hpp>
#include <mbgl/util/worker.hpp>
#include <mbgl/util/work_request.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/map/environment.hpp>

using namespace mbgl;

VectorTileData::VectorTileData(const TileID& id_,
                               Style& style_,
                               const SourceInfo& source_,
                               float angle,
                               bool collisionDebug)
    : TileData(id_),
      source(source_),
      env(Environment::Get()),
      worker(style_.workers),
      workerData(id_,
                 style_,
                 source.max_zoom,
                 state,
                 std::make_unique<CollisionTile>(id_.z, 4096,
                                    source_.tile_size * id.overscaling,
                                    angle, collisionDebug)),
      lastAngle(angle),
      currentAngle(angle) {
}

VectorTileData::~VectorTileData() {
    // Cancel in most derived class destructor so that worker tasks are joined before
    // any member data goes away.
    cancel();
}

void VectorTileData::request(Worker&,
                       float pixelRatio,
                       const std::function<void()>& callback) {
    std::string url = source.tileURL(id, pixelRatio);
    state = State::loading;

    req = env.request({ Resource::Kind::Tile, url }, [url, callback, this](const Response &res) {
        req = nullptr;

        if (res.status != Response::Successful) {
            std::stringstream message;
            message <<  "Failed to load [" << url << "]: " << res.message;
            setError(message.str());
            callback();
            return;
        }

        state = State::loaded;
        data = res.data;

        reparse(worker, callback);
    });
}

bool VectorTileData::reparse(Worker&, std::function<void()> callback) {
    if (!mayStartParsing()) {
        return false;
    }

    workRequest = worker.send([this] {
        if (getState() != TileData::State::loaded && getState() != TileData::State::partial) {
            return;
        }

        VectorTile vectorTile(pbf((const uint8_t *)data.data(), data.size()));
        TileParseResult result = workerData.parse(vectorTile);

        if (getState() == TileData::State::obsolete) {
            return;
        } else if (result.is<State>()) {
            setState(result.get<State>());
        } else {
            setError(result.get<std::string>());
        }

        endParsing();
    }, callback);

    return true;
}

Bucket* VectorTileData::getBucket(const StyleLayer& layer) {
    if (!isReady() || !layer.bucket) {
        return nullptr;
    }

    return workerData.getBucket(layer);
}

size_t VectorTileData::countBuckets() const {
    return workerData.countBuckets();
}

void VectorTileData::setState(const State& state_) {
    assert(!isImmutable());

    state = state_;

    if (isImmutable()) {
        workerData.collision->reset(0, 0);
    }
}

void VectorTileData::redoPlacement() {
    redoPlacement(lastAngle, lastCollisionDebug);
}

void VectorTileData::redoPlacement(float angle, bool collisionDebug) {
    if (angle != currentAngle || collisionDebug != currentCollisionDebug) {
        lastAngle = angle;
        lastCollisionDebug = collisionDebug;

        if (getState() != State::parsed || redoingPlacement) return;

        redoingPlacement = true;
        currentAngle = angle;
        currentCollisionDebug = collisionDebug;

        auto callback = std::bind(&VectorTileData::endRedoPlacement, this);
        workRequest = worker.send([this, angle, collisionDebug] { workerData.redoPlacement(angle, collisionDebug); }, callback);
    }
}

void VectorTileData::endRedoPlacement() {
    for (const auto& layer_desc : workerData.style.layers) {
        auto bucket = getBucket(*layer_desc);
        if (bucket) {
            bucket->swapRenderData();
        }
    }
    redoingPlacement = false;
    redoPlacement();
}

void VectorTileData::cancel() {
    if (state != State::obsolete) {
        state = State::obsolete;
    }
    if (req) {
        env.cancelRequest(req);
        req = nullptr;
    }
    workRequest.reset();
}

bool VectorTileData::mayStartParsing() {
    return !parsing.test_and_set(std::memory_order_acquire);
}

void VectorTileData::endParsing() {
    parsing.clear(std::memory_order_release);
}

void VectorTileData::setError(const std::string& message) {
    error = message;
    setState(State::obsolete);
}
