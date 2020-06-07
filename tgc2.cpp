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
ClassMeta::Alloc ClassMeta::alloc = nullptr;
ClassMeta::Dealloc ClassMeta::dealloc = nullptr;
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
  m->klass->callDealloc(m);
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

PtrBase::PtrBase() : isRoot(true), isOld(false) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  c->tryRegisterToClass(this);
}

PtrBase::PtrBase(void* obj) : isRoot(true), isOld(false) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();
  meta = c->globalFindOwnerMeta(obj);
  c->tryRegisterToClass(this);
}

PtrBase::~PtrBase() {
  auto* c = Collector::inst;
  c->unrefs.emplace_back(this);
}

void PtrBase::writeBarrier() {
  if (this->meta)
    Collector::inst->delayIntergenerationalPtrs.insert(this);
}

//////////////////////////////////////////////////////////////////////////

ObjMeta* ClassMeta::newMeta(size_t cnt) {
  auto* c = Collector::inst ? Collector::inst : Collector::get();

  if (c->allocCounter++ % c->newGenObjCntToGc == 0)
    c->collect();

  ObjMeta* meta = nullptr;
  try {
    isCreatingObj++;
    auto* p = callAlloc(size * cnt + sizeof(ObjMeta));
    meta = new (p) ObjMeta(this, p + sizeof(ObjMeta), cnt);
    // Allow using gc_from(this) in the constructor of the creating object.
    c->addMeta(meta);
    return meta;
  } catch (std::bad_alloc&) {
    if (meta)
      callDealloc(meta);
    throw;
  }
}

void ClassMeta::endNewMeta(ObjMeta* meta, bool failed) {
  auto* c = Collector::inst;
  isCreatingObj--;
  vector_erase(c->creatingObjs, meta);
  if (failed) {
    c->newGen.remove(meta);
    callDealloc(meta);
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
  config(1024 * 10, 0, 1024 * 10);
}

Collector::~Collector() {
  while (newGen.size()) {
    auto i = newGen.back();
    newGen.pop_back();
    delete i;
  }
  while (oldGen.size()) {
    auto i = oldGen.back();
    oldGen.pop_back();
    delete i;
  }
}

void Collector::config(int newGenObjCntToGc,
                       int oldGenObjCntToFullGc,
                       int tempSize) {
  if (!tempSize)
    tempSize = 1024 * 10;
  if (!oldGenObjCntToFullGc)
    oldGenObjCntToFullGc = newGenObjCntToGc * 10;

  this->newGenObjCntToGc = newGenObjCntToGc;
  this->oldGenObjCntToFullGc = oldGenObjCntToFullGc;

  roots.reserve(newGenObjCntToGc);
  unrefs.reserve(tempSize);
  temp.reserve(tempSize);
  intergenerationalPtrs.reserve(tempSize);
  delayIntergenerationalPtrs.reserve(1024 / 2);
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
  for (auto ptr : unrefs) {
    intergenerationalPtrs.erase(ptr);
    delayIntergenerationalPtrs.erase(ptr);
    roots.erase(ptr);
  }
  unrefs.clear();
}

void Collector::handleDelayIntergenerationalPtrs() {
  for (auto* p : delayIntergenerationalPtrs) {
    if (p->isRoot)
      roots.insert(p);
    else if (p->isOld) {
      intergenerationalPtrs.insert(p);
    }
  }
  delayIntergenerationalPtrs.clear();
}

ObjMeta* Collector::globalFindOwnerMeta(void* obj) {
  auto* meta = (ObjMeta*)((char*)obj - sizeof(ObjMeta));
  if (meta->magic == ObjMeta::Magic)
    return meta;
  else
    return nullptr;
}

void Collector::mark(ObjMeta* meta) {
  auto doMark = [&](ObjMeta* meta) {
    if (meta->color == ObjMeta::Color::White) {
      meta->color = ObjMeta::Color::Black;

      auto* ptrIt = meta->klass->enumPtrs(meta);
      for (; auto* child = ptrIt->getNext();) {
        if (auto* m = child->meta) {
          if (m->color == ObjMeta::Color::White)
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
void Collector::preMark(ObjMeta* meta) {
  auto work = [&](ObjMeta* meta) {
    // fix for circular references.
    if (meta->color == ObjMeta::Color::Black) {
      // sweep function cannot reset color of intergenerational objects.
      meta->color = ObjMeta::Color::White;

      auto* it = meta->klass->enumPtrs(meta);
      for (; auto* ptr = it->getNext();) {
        ptr->isRoot = false;
        if (auto* subMeta = ptr->meta) {
          // fix for circular references.
          if (subMeta->color == ObjMeta::Color::Black)
            temp.push_back(ptr->meta);
        }
      }
      delete it;
    }
  };

  work(meta);
  while (temp.size()) {
    auto* m = temp.back();
    temp.pop_back();
    work(m);
  }
}

void Collector::collectNewGen() {
  freeObjCntOfPrevGc = 0;
  newGenGcCount++;

  for (auto meta : newGen)
    preMark(meta);

  handleUnrefs();
  handleDelayIntergenerationalPtrs();

  for (auto ptr : roots) {
    if (ptr->meta && !ptr->isOld) {
      mark(ptr->meta);
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
  oldGen.push_back(meta);
  auto it = meta->klass->enumPtrs(meta);
  for (; auto* p = it->getNext();) {
    p->isOld = true;
    if (p->meta)
      intergenerationalPtrs.insert(p);
  }
  delete it;
}

void Collector::fullCollect() {
  freeObjCntOfPrevGc = 0;
  full = true;
  fullGcCount++;

  for (auto meta : newGen)
    preMark(meta);
  for (auto* meta : oldGen)
    preMark(meta);

  handleUnrefs();
  handleDelayIntergenerationalPtrs();

  for (auto ptr : roots) {
    if (ptr->meta) {
      mark(ptr->meta);
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
  printf("[new gen gc cnt ] %3d\n", newGenGcCount);
  printf("[full gen gc cnt] %3d\n", fullGcCount);
  printf("[last freed objs] %3d\n", freeObjCntOfPrevGc);
  printf("=======================\n");
}

}  // namespace details
}  // namespace tgc2
