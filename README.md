# TGC2

## A Tiny, precise, generational, mark & sweep, Garbage Collector for C++.

参考请注明出处，谢谢。

### Motivation
- Scenarios that shared_ptr can't solve, e.g. object dependencies are dynamically constructed with no chance to recognize the usage of shared & weak pointers.
- Try to make things simpler compared to shared_ptr and Oilpan, e.g. networking programs using callbacks for async io operations heavily.     
- A very good experiment to design a GC dedicated to the C++ language and see how the language features can help.    

### Highlights
- Non-intrusive
    - Use like shared_ptr.
    - Do not need to replace the global new operator.
    - Do not need to inherit from a common base.    
    - Can even work with shared_ptr.   
- Generational marking and sweeping
    - Can customize the trigger condtion of full gc.
    - Can manually delete the object to control the destruction order.
- Super lightweight    
    - Only one header & CPP file, easier to integrate.
    - No extra threads to collect garbage.    
- Support most of the containers of STL.        
- Cross-platform, no other dependencies, only dependent on STL.    
- Customization
    - It can work with other memory allocators and pool.
    - It can be extended to use your custom containers.    
- Precise.
    - Ensure no memory leaks as long as objects are correctly traced.

### Improvements compared with tgc
- better throughputs by using generational algorithm
- gc pointers can be global variables
- fast gc pointer construction
- support continuous vector
- unified way to handle class and containers
- gc pointer has smaller size
- do not support multi-inheritance
- can customize the condition of full collection



### Internals

### Performance Advice

### Usage

Please see the tests in 'test.cpp'.

### Refs

- https://www.codeproject.com/Articles/938/A-garbage-collection-framework-for-C-Part-II.
- Boehn GC: https://github.com/ivmai/bdwgc/
- Oilpan GC: https://chromium.googlesource.com/chromium/src/+/master/third_party/blink/renderer/platform/heap/BlinkGCDesign.md#Threading-model

### License

The MIT License

```
Copyright (C) 2018 soniced@sina.com

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
