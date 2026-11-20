# Ultra-fast snprintf() for microcontrollers #

I needed to debug a USB peripheral driver on an ARM Cortex-M4 based MCU.
Unfortunately the available sprintf()s were too slow and debug output caused
timeouts on the host. So I had Claude write me a faster one.

And it's really fast. I can now run my test suite with full debug logs.

It doesn't have all the features of libc's standard version. But it has
all the typical features required for logging. See sprintf.cpp for details
on what works and what doesn't. Should you need additional features, just
point the AI of your choice to this repository and ask it to add whatever
feature you need.

If you do create a variant of this sprintf with more/different features,
please take the time to make a pull request to contribute it to this
repository. Do not replace the original sprintf.cpp and sprintf_test.cpp
but add the variants with new filenames.

# Dependencies, C compatibility #

The file is written in C++ but the exported functions snprintf() and
vsnprintf() are export "C". So you can link sprintf.o as a drop-in
replacement for libc's versions, including with programs written in plain C.

sprintf.cpp has no library dependencies and does not allocate dynamic memory.

# Why C++ ? Why GCC ? #

I didn't want Claude to lose optimization opportunities by being forced into
a reduced feature set. So I told it to use C++ and GCC features if needed.

