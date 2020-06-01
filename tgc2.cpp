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

bool ObjMeta::operator<(ObjMeta& r) const {
  return objPtr() + klass->size * arrayLength <
         r.objPtr() + r.klass->size * r.arrayLength;
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

void PtrBase::onPtrChanged() {
  Collector::inst->onPointerChanged(this);
}

//////////////////////////////////////////////////////////////////////////

ObjMeta* ClassMeta::newMeta(size_t objCnt) {
  assert(memHandler && "should not be called in global scope (before main)");
  auto* meta = (ObjMeta*)memHandler(this, MemRequest::Alloc,
                                    reinterpret_cast<void*>(objCnt));

  try {
    auto* c = Collector::inst ? Collector::inst : Collector::get();
    // Allow using gc_from(this) in the constructor of the creating object.
    c->addMeta(meta);
  } catch (std::bad_alloc&) {
    memHandler(this, MemRequest::Dealloc, meta);
    throw;
  }

  isCreatingObj++;
  return meta;
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
  intergenerationalObjs.reserve(1024);
}

Collector::~Collector() {
  for (auto i = newGen.begin(); i != newGen.end();) {
    delete *i;
  }
  for (auto i = oldGen.begin(); i != oldGen.end();) {
    delete *i;
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
  p->meta->isRoot = p->isRoot();
  if (oldGen.find(p->owner) != oldGen.end()) {
    intergenerationalObjs.insert(p->meta);
  }

  if (newGen.size() > 1024)
    collectNewGen();
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

    for (auto it = meta->klass->enumPtrs(meta); auto* ptr = it->getNext();) {
      if (auto* meta = ptr->meta) {
        meta->isRoot = false;  // for containers
        if (meta->color == ObjMeta::Color::White) {
          meta->color = ObjMeta::Color::Black;
        }
      }
    }
  }
}

void Collector::collectNewGen() {
  for (auto meta : newGen) {
    if (meta->isRoot) {
      mark(meta);
    }
  }
  for (auto meta : intergenerationalObjs) {
    mark(meta);
  }

  freeObjCntOfPrevGc = 0;
  sweep(newGen, true);
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

      if (full && meta->scanCountInNewGen++ > 3) {
        promote(meta);
        meta->scanCountInNewGen = 0;
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
    if (meta->isRoot) {
      mark(meta);
    }
  }
  for (auto* meta : oldGen) {
    if (meta->isRoot) {
      mark(meta);
    }
  }
  freeObjCntOfPrevGc = 0;
  sweep(newGen, false);
  oldGenSize -= sweep(oldGen, false);
}

void Collector::collect() {
  collectNewGen();
  if (oldGenSize > sizeOfOldGenToFullCollect) {
    fullCollect();
  }
}

void Collector::dumpStats() {
  printf("========= [gc] ========\n");
  printf("[newGen meta     ] %3d\n", (unsigned)newGen.size());
  printf("[oldGen meta     ] %3d\n", (unsigned)oldGen.size());
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
