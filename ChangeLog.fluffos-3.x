Wodan announced 3.x release will be maintained by
"Yucong Sun" (sunyucong@gmail.com) for now.

Please submit issue to https://github.com/fluffos/fluffos

Major changes compare to 2.x:

Build:
  * FluffOS has switched to c++ language, C++11 (G++ 4.6+, CLANG 2.9+) is required to build.
  * FluffOS now use C++ try/catch instead of setjmp/longjmp for LPC error recovery. (wodan)
  * FluffOS now uses autoconf for dependency configuraiton. (alpha4 done)
  * Libevent 2.0+ is required to build, by default epoll() is used instead of select(). (alpha6)
  * all *.c files has been renamed to *.cc. all *_spec.c files has been renamed to
      *.spec . (alpha6)

EFUN/Package changes:
  * limit for number of total EFUNs has been raised to 65535 (alpha2)
  * unique_mapping() no longer leak memory.
  * Use c++11 mt19937_64 as driver random number generator engine. quality of random() is improved.
  * PACKAGE_CONTRIB: store_variable | fetch_variable accept an object as 3rd last argument. (Lonely@NT)
  * PACKAGE_CRYPTO: build fixes and enhancements. (voltara@lpmuds.net)
  * PACKAGE_SHA1: Fix incorrect sha1() hash generation, verified with tests. (voltara@lpmuds.net)

New compile options/packages:
  * PACKAGE_TRIM: (zoilder), rtrim, ltrim, and trim for string trimming.
  * POSIX_TIMERS: better time preceision tracking for eval cost. (voltara@lpmuds.net)
  * CALLOUT_LOOP_PROTECTION: protect call_out(0) loops. (voltara@lpmuds.net)
  * SANE_SORTING: Use faster sorting implementation for "sort_array()", but requires
                  LPC code to return conforming results.
  * REVERSE_DEFER: fifo execution order for defer() efun (default to lifo)

Misc:
  * FluffOS now provide 64bit LPC runtime regardless of host system. (including 32bit linux/CYGWIN).
  * addr_server is now obsolete and deleted, the functionaltiy is built-in. (alpha6)
  * DEBUG_MACRO is now always true, the functionaltiy is being canonicalized as built-in.
  * Various bug/crash fixes.

TODOs:
  Use event backend to schedule call_out, heart_beat and object swap/reclaim routine.
  remove SQLITE2 support from package/db.c
  bundle with google-glog library.
  LPC JIT compiler (bundle with LLVM).
  switch to automake.

Known Issues:
  "-MAX_INT" is not parsed correctly in LPC(pre-existing bug), see
    src/testsuite/single/tests/64bit.c for details.

================================================================================
  Per-release ChangeLog
================================================================================
FluffOS 3.0-alpha6.4
  * Fixed efun present() issue for object id that ends with digits, added test.

FluffOS 3.0-alpha6.3
  * Fixed memory corruption issue for PACKAGE_CRYPTO, added a test.

FluffOS 3.0-alpha6.2

  * Fixed memory corruption issue for store_variable(), added test.
  * Enable TCP_NODELAY on user socket and lpc socket by default.

FluffOS 3.0-alpha6.1

Major Stuff:
  * Libevent 2.0 integration: epoll() for user and lpc sockets!
  * Async DNS: Driver will do async dns directly, addr_server support is now obsolete and deleted.
  * all *.c files has been changed to *.cc. all *_spec.c files has been changed to
      *.spec . (If you are maintaining out of tree patches, be aware!)
  * DEBUG_MACRO is now always true, the functionaltiy is being canonicalized as built-in
    behavior. You can run "./driver -d<type>" to show vairous debug message in the driver. (or call
    efun in debug.c, console support is coming later).
    Current choice of <type> includes:
       * "-dconnections", shows all player connection related events.
       * "-devent", shows internal event loop messages.
       * "-ddns", shows async dns resolution messages.
       * "-dsockets", shows all lpc socket related messages.
       * "-dLPC", shows current LPC execution informations.
       * "-dLPC_line", shows current LPC line being executed.
       * "-dfile", shows some file related messages.
       * empty (aka. "-d"), this currently shows connections + dns + sockets messages, will change later.
       * other debug message is not yet fully categorized, like "-dflag".
  * Use c++11 mt19937_64 as random number generator engine.

EFUN:
  * PACKAGE_CONTRIB: store_variable | fetch_variable accept an object as 3rd last argument. (Lonely@NT)

Other:
  * Restore "Mud IP" config line in testsuite, and fixed socket related code to be ip-agnostic.
  * Delete all old win32/windows code. And obsolote malloc implementations, the only choice now is
    SYSMALLOC (preferred), 32bitmalloc and 64bitmalloc.
  * Most of the internal socket related code is now ip-agnostic.
  * Force creating dwarf-2 debug info format.
  * Make new_user_handler() safe from LPC error().
  * Add configure check for C++ 11.
  * Allow passing CXXFLAGS to build.FluffOS.
  * Fix crash when sending 0 length message through lpc socket.
  * Fix buffer underrun in present("1").
  * deleted text/html version of document, moved nroff version to /doc.
  * add back "-flto" compile option.

FluffOS 3.0-alpha5
    REVERSE_DEFER, fifo execution for defer()

    Rewrite unique_mapping(), no more memory leak.
    Adding a test for unique_mapping.
    make DEBUG driver skip graceful crash routine.
    Fix ltrim bug (zolider), adding more test cases.
    Fix broken get_usec_clock (time_expression).
    Also remove some obsolete signal code.
    Fix build with PCRE, MYSQL, PGSQL, SQLITE3.

FluffOS 3.0-alpha4
  PACKAGE_TRIM: (zoilder), rtrim, ltrim, and trim for string trimming.

  FluffOS has switched to use autoconf for compatibility detection.
  This has lead to removal of almost half of the code in edit_source.c
  Note: the correct way to build is still to launch ./build.FluffOS first,
    then make.
  Other general code quality imporvements, many old crafts has been removed.
  FluffOS 3.0 will only support mondern linux distributions.

FluffOS 3.0-alpha3
  FluffOS has switched to c++ language.
  use try/catch instead of longjmp. (wodan)
  Code quality improvment.
  Fix using DEBUG without DEBUGMALLOC_EXTENSIONS cause memory corruption.

FluffOS 3.0-alpha2

General:
  Rebased to 2.27 released by wodan.
  Build will fail early when local_options is missing.
  Enforce source format using astyle.

BugFix:
  command() efun will correctly return eval cost.
  Crasher 14, crash when returning array types.

Tests:
  Tests are now executed randomly.
  debugmalloc will fill memory with magic values.

FluffOS 3.0-alpha1

New compile options:
  POSIX_TIMERS: better time preceision tracking for eval cost. (voltara@lpmuds.net)
  CALLOUT_LOOP_PROTECTION: protect call_out(0) loops. (voltara@lpmuds.net)
  SANE_SORTING: Use faster sorting implementation for "sort_array()", but requires
                LPC code to return conforming results.

General:
  Build script improvement and compile/warning fixes.
  Build under 32bit environment is now supported.
  Build under CYGWIN is now supported.
  Multiple crasher/memory leaks is fixed.
  Documentation has been moved to root directory.
  Enable Travis CI to automate test/build for each commit.
  Auto print backtrace dump when driver crash.
  Print warning on startup if core dump limit is 0.
  Compile fix for db.c without PACKAGE_ASYNC. (mactorg@lpmuds.net)
  General code quality improvements.

Packages:
  PACKAGE_CRYPTO: build fixes and enhancements. (voltara@lpmuds.net)
  PACKAGE_SHA1: Fix incorrect sha1() hash generation, verified with tests.
                (voltara@lpmuds.net)

Test:
  "make test" will launch testsuite and report any problems.
  DEBUGMALLOC, DEBUGMALLOC_EXTENSIONS and CHECK_MEMORY is working now.
  Extensive 64bit runtime tests is added.
  Switch operator tests.
  Benchmarker and auto-crasher improvments.

LPC:
  LPC runtime is strictly 64bit now and everything should conform.
  MIN_INT, MAX_FLOAT, MIN_FLOAT predefines.
  Maximum number of EFUNs has been raised to 65535 from 256.

Known Issues:
  "-MAX_INT" is not parsed correctly in LPC(old bug), see
    src/testsuite/single/tests/64bit.c for details.
  unique_mapping() EFUN will leak memory.
  crasher in testsuite needs improvements.
