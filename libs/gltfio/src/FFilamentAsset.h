/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GLTFIO_FFILAMENTASSET_H
#define GLTFIO_FFILAMENTASSET_H

#include <gltfio/FilamentAsset.h>

#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>

#include <gltfio/MaterialProvider.h>

#include <math/mat4.h>

#include <utils/FixedCapacityVector.h>
#include <utils/CString.h>
#include <utils/Entity.h>

#include <cgltf.h>

#include "upcast.h"
#include "DependencyGraph.h"
#include "DracoCache.h"
#include "FFilamentInstance.h"

#include <tsl/robin_map.h>
#include <tsl/htrie_map.h>

#include <vector>

#ifdef NDEBUG
#define GLTFIO_VERBOSE 0
#define GLTFIO_WARN(msg)
#else
#define GLTFIO_VERBOSE 1
#define GLTFIO_WARN(msg) slog.w << msg << io::endl
#endif

namespace utils {
    class NameComponentManager;
    class EntityManager;
}

namespace gltfio {

class Animator;
class Wireframe;
class MorphHelper;

// Encapsulates VertexBuffer::setBufferAt() or IndexBuffer::setBuffer().
struct BufferSlot {
    const cgltf_accessor* accessor;
    cgltf_attribute_type attribute;
    int bufferIndex; // for vertex buffers only
    filament::VertexBuffer* vertexBuffer;
    filament::IndexBuffer* indexBuffer;
};

// Encapsulates a connection between Texture and MaterialInstance.
struct TextureSlot {
    const cgltf_texture* texture;
    filament::MaterialInstance* materialInstance;
    const char* materialParameter;
    filament::TextureSampler sampler;
    bool srgb;
};

// MeshCache
// ---------
// If a given glTF mesh is referenced by multiple glTF nodes, then it generates a separate Filament
// renderable for each of those nodes. All renderables generated by a given mesh share a common set
// of VertexBuffer and IndexBuffer objects. To achieve the sharing behavior, the loader maintains a
// small cache. The cache keys are glTF mesh definitions and the cache entries are lists of
// primitives, where a "primitive" is a reference to a Filament VertexBuffer and IndexBuffer.
struct Primitive {
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    filament::Aabb aabb; // object-space bounding box
    UvMap uvmap; // mapping from each glTF UV set to either UV0 or UV1 (8 bytes)
};
using MeshCache = tsl::robin_map<const cgltf_mesh*, std::vector<Primitive>>;

// MatInstanceCache
// ----------------
// Each glTF material definition corresponds to a single filament::MaterialInstance, which are
// temporarily cached during loading. The filament::Material objects that are used to create instances are
// cached in MaterialProvider. If a given glTF material is referenced by multiple glTF meshes, then
// their corresponding filament primitives will share the same Filament MaterialInstance and UvMap.
// The UvMap is a mapping from each texcoord slot in glTF to one of Filament's 2 texcoord sets.
struct MaterialEntry {
    filament::MaterialInstance* instance;
    UvMap uvmap;
};
using MatInstanceCache = tsl::robin_map<intptr_t, MaterialEntry>;

struct FFilamentAsset : public FilamentAsset {
    FFilamentAsset(filament::Engine* engine, utils::NameComponentManager* names,
            utils::EntityManager* entityManager, const cgltf_data* srcAsset) :
            mEngine(engine), mNameManager(names), mEntityManager(entityManager) {
        mSourceAsset.reset(new SourceAsset {(cgltf_data*)srcAsset});
    }

    ~FFilamentAsset();

    size_t getEntityCount() const noexcept {
        return mEntities.size();
    }

    const utils::Entity* getEntities() const noexcept {
        return mEntities.empty() ? nullptr : mEntities.data();
    }

    const utils::Entity* getLightEntities() const noexcept {
        return mLightEntities.empty() ? nullptr : mLightEntities.data();
    }

    size_t getLightEntityCount() const noexcept {
        return mLightEntities.size();
    }

    const utils::Entity* getCameraEntities() const noexcept {
        return mCameraEntities.empty() ? nullptr : mCameraEntities.data();
    }

    size_t getCameraEntityCount() const noexcept {
        return mCameraEntities.size();
    }

    utils::Entity getRoot() const noexcept {
        return mRoot;
    }

    size_t popRenderables(utils::Entity* entities, size_t count) noexcept {
        return mDependencyGraph.popRenderables(entities, count);
    }

    size_t getMaterialInstanceCount() const noexcept {
        return mMaterialInstances.size();
    }

    const filament::MaterialInstance* const* getMaterialInstances() const noexcept {
        return mMaterialInstances.data();
    }

    filament::MaterialInstance* const* getMaterialInstances() noexcept {
        return mMaterialInstances.data();
    }

    size_t getResourceUriCount() const noexcept {
        return mResourceUris.size();
    }

    const char* const* getResourceUris() const noexcept {
        return mResourceUris.data();
    }

    filament::Aabb getBoundingBox() const noexcept {
        return mBoundingBox;
    }

    const char* getName(utils::Entity entity) const noexcept;

    const char* getExtras(utils::Entity entity) const noexcept;

    utils::Entity getFirstEntityByName(const char* name) noexcept;

    size_t getEntitiesByName(const char* name, utils::Entity* entities,
            size_t maxCount) const noexcept;

    size_t getEntitiesByPrefix(const char* prefix, utils::Entity* entities,
            size_t maxCount) const noexcept;

    Animator* getAnimator() noexcept;

    const char* getMorphTargetNameAt(utils::Entity entity, size_t targetIndex) const noexcept;

    utils::Entity getWireframe() noexcept;

    filament::Engine* getEngine() const noexcept {
        return mEngine;
    }

    void releaseSourceData() noexcept;

    const void* getSourceAsset() const noexcept {
        return mSourceAsset.get() ? mSourceAsset->hierarchy : nullptr;
    }

    FilamentInstance** getAssetInstances() noexcept {
        return (FilamentInstance**) mInstances.data();
    }

    size_t getAssetInstanceCount() const noexcept {
        return mInstances.size();
    }

    void takeOwnership(filament::Texture* texture) {
        mTextures.push_back(texture);
    }

    void bindTexture(const TextureSlot& tb, filament::Texture* texture) {
        tb.materialInstance->setParameter(tb.materialParameter, texture, tb.sampler);
        mDependencyGraph.addEdge(texture, tb.materialInstance, tb.materialParameter);
    }

    bool isInstanced() const {
        return mInstances.size() > 0;
    }

    filament::Engine* mEngine;
    utils::NameComponentManager* mNameManager;
    utils::EntityManager* mEntityManager;
    std::vector<utils::Entity> mEntities;
    std::vector<utils::Entity> mLightEntities;
    std::vector<utils::Entity> mCameraEntities;
    std::vector<filament::MaterialInstance*> mMaterialInstances;
    std::vector<filament::VertexBuffer*> mVertexBuffers;
    std::vector<filament::BufferObject*> mBufferObjects;
    std::vector<filament::IndexBuffer*> mIndexBuffers;
    std::vector<filament::Texture*> mTextures;
    filament::Aabb mBoundingBox;
    utils::Entity mRoot;
    std::vector<FFilamentInstance*> mInstances;
    SkinVector mSkins; // unused for instanced assets
    Animator* mAnimator = nullptr;
    MorphHelper* mMorpher = nullptr;
    Wireframe* mWireframe = nullptr;
    bool mResourcesLoaded = false;
    DependencyGraph mDependencyGraph;
    tsl::htrie_map<char, std::vector<utils::Entity>> mNameToEntity;
    tsl::robin_map<utils::Entity, utils::CString> mNodeExtras;
    utils::CString mAssetExtras;

    // Sentinels for situations where ResourceLoader needs to generate data.
    const cgltf_accessor mGenerateNormals = {};
    const cgltf_accessor mGenerateTangents = {};

    // Encapsulates reference-counted source data, which includes the cgltf hierachy
    // and potentially also includes buffer data that can be uploaded to the GPU.
    struct SourceAsset {
        ~SourceAsset() { cgltf_free(hierarchy); }
        cgltf_data* hierarchy;
        DracoCache dracoCache;
        utils::FixedCapacityVector<uint8_t> glbData;
    };

    // We used shared ownership for the raw cgltf data in order to permit ResourceLoader to
    // complete various asynchronous work (e.g. uploading buffers to the GPU) even after the asset
    // or ResourceLoader have been destroyed.
    using SourceHandle = std::shared_ptr<SourceAsset>;
    SourceHandle mSourceAsset;

    // Transient source data that can freed via releaseSourceData:
    std::vector<BufferSlot> mBufferSlots;
    std::vector<TextureSlot> mTextureSlots;
    std::vector<const char*> mResourceUris;
    NodeMap mNodeMap; // unused for instanced assets
    std::vector<std::pair<const cgltf_primitive*, filament::VertexBuffer*> > mPrimitives;
    MatInstanceCache mMatInstanceCache;
    MeshCache mMeshCache;
};

FILAMENT_UPCAST(FilamentAsset)

} // namespace gltfio

#endif // GLTFIO_FFILAMENTASSET_H
