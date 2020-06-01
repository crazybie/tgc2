#include "tgc2.h"

#ifdef _WIN32
#include <crtdbg.h>
#endif

namespace tgc2 {
namespace details {

int ClassMeta::isCreatingObj = 0;

Collector* Collector::inst = nullptr;

//////////////////////////////////////////////////////////////////////////

char* ObjMeta::objPtr() const {
  return (char*)this + sizeof(ObjMeta);
}

void ObjMeta::destroy() {
  if (!arrayLength)
    return;
  klass->memHandler(klass, ClassMeta::MemRequest::Dctor, this);
  arrayLength = 0;
}

size_t ObjMeta::sizeInBytes() const {
  return arrayLength * klass->size;
}

void ObjMeta::operator delete(void* p) {
  auto* m = (ObjMeta*)p;
  m->klass->memHandler(m->klass, ClassMeta::MemRequest::Dealloc, m);
}

bool ObjMeta::containsPtr(char* p) {
  auto* o = objPtr();
  return o <= p && p < o + klass->size * arrayLength;
}

//////////////////////////////////////////////////////////////////////////

const PtrBase* ObjPtrEnumerator::getNext() {
  if (auto* subPtrs = meta->klass->subPtrOffsets) {
    if (arrayElemIdx < meta->arrayLength && subPtrIdx < subPtrs->size()) {
      auto* klass = meta->klass;
      auto* obj = meta->objPtr() + arrayElemIdx * klass->size;
      auto* subPtr = obj + (*klass->subPtrOffsets)[subPtrIdx];
      if (subPtrIdx++ >= klass->subPtrOffsets->size())
        arrayElemIdx++;
      return (PtrBase*)subPtr;
    }
  }
  return nullptr;
}

//////////////////////////////////////////////////////////////////////////

PtrBase::PtrBase() {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  c->registerToOwnerClass(this);
}

PtrBase::PtrBase(void* obj) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  meta = c->globalFindOwnerMeta(obj);
  c->registerToOwnerClass(this);
}

PtrBase::~PtrBase() {
  if (isRoot()) {
    if (meta && meta->rootRefs > 0)
      meta->rootRefs--;
  }
}

void PtrBase::onPtrChanged() {
  Collector::inst->onPointerChanged(this);
}

//////////////////////////////////////////////////////////////////////////

ObjMeta* ClassMeta::newMeta(size_t cnt) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  if (c->newGen.size() % c->newGenSizeForCollect == 0)
    c->collect();

  ObjMeta* meta = nullptr;
  try {
    assert(memHandler && "should not be called in global scope (before main)");
    if (memHandler) {
      isCreatingObj++;
      meta = (ObjMeta*)memHandler(this, MemRequest::Alloc,
                                  reinterpret_cast<void*>(cnt));

      // Allow using gc_from(this) in the constructor of the creating object.
      c->addMeta(meta);
    }
    return meta;
  } catch (std::bad_alloc&) {
    memHandler(this, MemRequest::Dealloc, meta);
    throw;
  }
}

void ClassMeta::endNewMeta(ObjMeta* meta, bool failed) {
  auto* c = Collector::inst;

  isCreatingObj--;
  if (!failed) {
    state = ClassMeta::State::Registered;
  }

  {
    c->creatingObjs.remove(meta);
    if (failed) {
      c->newGen.erase(meta);
      memHandler(this, MemRequest::Dealloc, meta);
    }
  }
}

void ClassMeta::registerSubPtr(ObjMeta* owner, PtrBase* p) {
  auto offset = (OffsetType)((char*)p - owner->objPtr());
  {
    if (state == ClassMeta::State::Registered)
      return;
    // constructor recursed.
    if (subPtrOffsets && offset <= subPtrOffsets->back())
      return;
  }

  if (!subPtrOffsets)
    subPtrOffsets = new vector<OffsetType>();
  subPtrOffsets->push_back(offset);
}

//////////////////////////////////////////////////////////////////////////

Collector::Collector() {
  newGen.reserve(1024 * 10);
  oldGen.reserve(1024 * 10);
  intergenerationalObjs.reserve(1024 * 10);
}

Collector::~Collector() {
  for (auto i : newGen) {
    delete i;
  }
  for (auto i : oldGen) {
    delete i;
  }
}

Collector* Collector::get() {
  if (!inst) {
#ifdef _WIN32
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    inst = new Collector();
    atexit([] { delete inst; });
  }
  return inst;
}

void Collector::addMeta(ObjMeta* meta) {
  newGen.insert(meta);
  creatingObjs.push_back(meta);
}

void Collector::registerToOwnerClass(PtrBase* p) {
  if (ClassMeta::isCreatingObj > 0) {
    if (auto* owner = findCreatingObj(p)) {
      p->owner = owner;
      owner->klass->registerSubPtr(owner, p);
    }
  }
}

void Collector::onPointerChanged(PtrBase* p) {
  if (!p->meta)
    return;
  if (p->isRoot())
    p->meta->rootRefs++;
  else if (oldGen.find(p->owner) != oldGen.end()) {
    intergenerationalObjs.insert(p->owner);
  }
}

ObjMeta* Collector::findCreatingObj(PtrBase* p) {
  // owner may not be the current one(e.g. constructor recursed)
  for (auto i = creatingObjs.rbegin(); i != creatingObjs.rend(); ++i) {
    if ((*i)->containsPtr((char*)p))
      return *i;
  }
  return nullptr;
}

ObjMeta* Collector::globalFindOwnerMeta(void* obj) {
  auto* meta = (ObjMeta*)((char*)obj - sizeof(ObjMeta));
  return meta;
}

void Collector::mark(ObjMeta* meta) {
  if (meta->color == ObjMeta::Color::White) {
    meta->color = ObjMeta::Color::Black;
    markChildren(meta);
  }
}

void Collector::markChildren(ObjMeta* meta) {
  auto* it = meta->klass->enumPtrs(meta);
  for (; auto* child = it->getNext();) {
    if (auto* m = child->meta) {
      m->rootRefs = 0;  // for container elements.
      m->color = ObjMeta::Color::Black;
    }
  }
  delete it;
}

void Collector::collectNewGen() {
  for (auto meta : newGen) {
    // for containers
    IPtrEnumerator* it;
    for (it = meta->klass->enumPtrs(meta); auto* ptr = it->getNext();) {
      if (auto* e = ptr->meta) {
        e->rootRefs = 0;
      }
    }
    delete it;

    if (meta->isRoot()) {
      mark(meta);
    }
  }

  for (auto meta : intergenerationalObjs) {
    markChildren(meta);
  }

  sweep(newGen, false);
}

int Collector::sweep(MetaSet& gen, bool full) {
  int freeSizeBytes = 0;

  for (auto it = gen.begin(); it != gen.end();) {
    auto* meta = *it;

    if (meta->color == ObjMeta::Color::White) {
      freeSizeBytes += meta->sizeInBytes();
      freeObjCntOfPrevGc++;

      delete meta;
      if (full)
        intergenerationalObjs.erase(meta);

      it = gen.erase(it);
    } else {
      meta->color = ObjMeta::Color::White;

      if (!full && ++meta->scanCountInNewGen >= oldGenScanCount) {
        meta->scanCountInNewGen = 0;

        promote(meta);
        it = newGen.erase(it);
      } else
        ++it;
    }
  }
  return freeSizeBytes;
}

void Collector::promote(ObjMeta* meta) {
  oldGen.insert(meta);
  intergenerationalObjs.insert(meta);
  oldGenSize += meta->sizeInBytes();
}

void Collector::fullCollect() {
  for (auto meta : newGen) {
    if (meta->isRoot()) {
      mark(meta);
    }
  }
  for (auto* meta : oldGen) {
    if (meta->isRoot()) {
      mark(meta);
    }
  }

  sweep(newGen, true);
  oldGenSize -= sweep(oldGen, true);
}

void Collector::collect() {
  freeObjCntOfPrevGc = 0;
  collectNewGen();
  if (oldGenSize > sizeOfOldGenToFullCollect) {
    fullCollect();
  }
}

void Collector::dumpStats() {
  printf("========= [gc] ========\n");
  printf("[newGen meta    ] %3d\n", (unsigned)newGen.size());
  printf("[oldGen meta    ] %3d\n", (unsigned)oldGen.size());
  auto liveCnt = 0;
  for (auto i : newGen)
    if (i->arrayLength)
      liveCnt++;
  for (auto i : oldGen)
    if (i->arrayLength)
      liveCnt++;
  printf("[live objects   ] %3d\n", liveCnt);
  printf("[last freed objs] %3d\n", freeObjCntOfPrevGc);
  printf("=======================\n");
}

}  // namespace details
}  // namespace tgc2
