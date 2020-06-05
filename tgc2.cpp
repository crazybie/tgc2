#include "tgc2.h"

#include <algorithm>

#ifdef _WIN32
#include <crtdbg.h>
#endif

namespace tgc2 {
namespace details {

template <typename T>
void vector_erase(vector<T>& c, T& v) {
  c.erase(remove(c.begin(), c.end(), v), c.end());
}

int ClassMeta::isCreatingObj = 0;

Collector* Collector::inst = nullptr;

char IPtrEnumerator::buf[255];

//////////////////////////////////////////////////////////////////////////

void ObjMeta::destroy() {
  if (!arrayLength)
    return;
  klass->memHandler(klass, ClassMeta::MemRequest::Dctor, this);
  arrayLength = 0;
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

PtrBase* ObjPtrEnumerator::getNext() {
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
  c->tryRegisterToClass(this);
}

PtrBase::PtrBase(void* obj) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  meta = c->globalFindOwnerMeta(obj);
  c->tryRegisterToClass(this);
}

PtrBase::~PtrBase() {
  auto* c = Collector::inst;
  c->unrefs.emplace_back(this, meta);
}

void PtrBase::writeBarrier() {
  if (this->meta)
    Collector::inst->delayIntergenerationalPtrs.insert(this);
}

//////////////////////////////////////////////////////////////////////////

ObjMeta* ClassMeta::newMeta(size_t cnt) {
  assert(memHandler && "should not be called in global scope (before main)");

  auto* c = Collector::inst ? Collector::inst : Collector::get();

  if (c->allocCounter++ % c->newGenObjCntToGc == 0)
    c->collect();

  ObjMeta* meta = nullptr;
  try {
    if (memHandler) {
      isCreatingObj++;
      meta = (ObjMeta*)memHandler(this, MemRequest::Alloc,
                                  reinterpret_cast<void*>(cnt));
      // Allow using gc_from(this) in the constructor of the creating object.
      c->addMeta(meta);
    }
    return meta;
  } catch (std::bad_alloc&) {
    if (meta)
      memHandler(this, MemRequest::Dealloc, meta);
    throw;
  }
}

void ClassMeta::endNewMeta(ObjMeta* meta, bool failed) {
  auto* c = Collector::inst;
  isCreatingObj--;
  {
    vector_erase(c->creatingObjs, meta);
    if (failed) {
      c->newGen.remove(meta);
      memHandler(this, MemRequest::Dealloc, meta);
    }
  }
}

void ClassMeta::registerSubPtr(ObjMeta* owner, PtrBase* p) {
  auto offset = (OffsetType)((char*)p - owner->objPtr());
  if (!subPtrOffsets) {
    subPtrOffsets = new vector<OffsetType>();
    subPtrOffsets->push_back(offset);
  } else if (offset > subPtrOffsets->back())
    subPtrOffsets->push_back(offset);
}

//////////////////////////////////////////////////////////////////////////

Collector::Collector() {
  reserve(1024 * 10);
}

Collector::~Collector() {
  for (auto i : newGen) {
    delete i;
  }
  for (auto i : oldGen) {
    delete i;
  }
}

void Collector::reserve(int sz) {
  // newGen.reserve(sz);
  // oldGen.reserve(sz * 10);
  unrefs.reserve(sz * 10);
  temp.reserve(sz);
  intergenerationalPtrs.reserve(1024 * 10);
  delayIntergenerationalPtrs.reserve(1024 * 10);
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
  newGen.push_back(meta);
  creatingObjs.push_back(meta);
}

void Collector::tryRegisterToClass(PtrBase* p) {
  if (ClassMeta::isCreatingObj > 0) {
    // owner may not be the current one(e.g. constructor recursed)
    for (auto i = creatingObjs.rbegin(); i != creatingObjs.rend(); ++i) {
      auto* owner = *i;
      if (owner->containsPtr((char*)p)) {
        owner->klass->registerSubPtr(owner, p);
      }
    }
  }
}

void Collector::handleUnrefs() {
  for (auto& [ptr, meta] : unrefs) {
    intergenerationalPtrs.erase(ptr);
    delayIntergenerationalPtrs.erase(ptr);

    if (meta && meta->refCntFromRoot > 0)
      meta->refCntFromRoot--;
  }
  unrefs.clear();
}

void Collector::handleDelayIntergenerationalPtrs() {
  for (auto* p : delayIntergenerationalPtrs) {
    if (p->isRoot())
      p->meta->refCntFromRoot++;
    else if (p->owner->isOld) {
      intergenerationalPtrs.insert(p);
    }
  }
  delayIntergenerationalPtrs.clear();
}

ObjMeta* Collector::globalFindOwnerMeta(void* obj) {
  auto* meta = (ObjMeta*)((char*)obj - sizeof(ObjMeta));
  return meta;
}

void Collector::mark(ObjMeta* meta) {
  auto doMark = [&](ObjMeta* meta) {
    if (meta->color == ObjMeta::Color::White) {
      meta->color = ObjMeta::Color::Black;

      auto* ptrIt = meta->klass->enumPtrs(meta);
      for (; auto* child = ptrIt->getNext();) {
        if (auto* m = child->meta) {
          temp.push_back(m);
        }
      }
      delete ptrIt;
    }
  };

  doMark(meta);
  while (temp.size()) {
    auto* m = temp.back();
    temp.pop_back();
    doMark(m);
  }
}

// Unified way for objects and containers.
// Need to fix for every pass as containers can be modified at any time.
void Collector::fixOwner(ObjMeta* meta) {
  auto doFix = [&](ObjMeta* meta) {
    // sweep function cannot reset color of intergenerational objects.
    meta->color = ObjMeta::Color::White;

    auto* it = meta->klass->enumPtrs(meta);
    for (; auto* ptr = it->getNext();) {
      ptr->owner = meta;
      if (ptr->meta) {
        ptr->meta->refCntFromRoot = 0;
        temp.push_back(ptr->meta);
      }
    }
    delete it;
  };

  doFix(meta);
  while (temp.size()) {
    auto* m = temp.back();
    temp.pop_back();
    doFix(m);
  }
}

void Collector::collectNewGen() {
  freeObjCntOfPrevGc = 0;

  for (auto meta : newGen)
    fixOwner(meta);

  handleUnrefs();
  handleDelayIntergenerationalPtrs();

  for (auto meta : newGen) {
    if (meta->isRoot()) {
      mark(meta);
    }
  }

  for (auto ptr : intergenerationalPtrs) {
    mark(ptr->meta);
  }

  sweep(newGen);
}

void Collector::sweep(MetaSet& gen) {
  for (auto it = gen.begin(); it != gen.end();) {
    auto* meta = *it;

    if (meta->color == ObjMeta::Color::White) {
      freeObjCntOfPrevGc++;
      it = gen.erase(it);
      delete meta;
    } else {
      if (!full && ++meta->scanCountInNewGen >= scanCountToOldGen) {
        meta->scanCountInNewGen = 0;
        it = newGen.erase(it);
        promote(meta);
      } else
        ++it;
    }
  }

  if (trace)
    printf("sweep %s, free cnt:%d\n", &gen == &oldGen ? "old" : "new",
           freeObjCntOfPrevGc);
}

void Collector::promote(ObjMeta* meta) {
  meta->isOld = true;
  oldGen.push_back(meta);
  auto it = meta->klass->enumPtrs(meta);
  for (; auto* p = it->getNext();) {
    if (p->meta)
      intergenerationalPtrs.insert(p);
  }
  delete it;
}

void Collector::fullCollect() {
  freeObjCntOfPrevGc = 0;
  full = true;

  for (auto meta : newGen)
    fixOwner(meta);
  for (auto* meta : oldGen)
    fixOwner(meta);

  handleUnrefs();
  handleDelayIntergenerationalPtrs();

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

  sweep(newGen);
  sweep(oldGen);
  full = false;
}

void Collector::collect() {
  if (oldGen.size() > oldGenObjCntToFullGc) {
    fullCollect();
  } else {
    collectNewGen();
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
