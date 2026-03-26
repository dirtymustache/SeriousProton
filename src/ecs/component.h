#pragma once

#include "container/sparseset.h"
#include "logging.h"


namespace sp::ecs {
template<typename T> class ComponentReplication;

class ComponentStorageBase {
public:
    ComponentStorageBase();

    static void dumpDebugInfo();
private:
    ComponentStorageBase* next = nullptr;
protected:
    static void destroyAll(uint32_t index);
    virtual void destroy(uint32_t index) = 0;
    virtual void dumpDebugInfoImpl() = 0;

    friend class Entity;
};

template<typename T> class ComponentStorage : public ComponentStorageBase {
public:
    static ComponentStorage<T>& instance()
    {
        static ComponentStorage<T> storage;
        return storage;
    }

private:
    ComponentStorage() = default;

    void destroy(uint32_t index) override
    {
        sparseset.remove(index);
    }

    virtual void dumpDebugInfoImpl() override
    {
        if (sparseset.size())
            LOG(Debug, "Component:", typeid(T).name(), " Count: ", sparseset.size());
    }

    SparseSet<T> sparseset;

    friend class Entity;
    template<class, class...> friend class Query;
    friend class ComponentReplication<T>;
};

}
