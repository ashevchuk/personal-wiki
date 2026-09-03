# Test targets land in Milestone 1 (PathGuard traversal tests are the first
# ones written, per the plan — security-critical code gets tests before
# anything else touches the filesystem). Framework choice (Catch2 via vcpkg)
# is decided then; this file exists now so `add_subdirectory(tests)` in the
# root CMakeLists.txt is valid from M0 onward.
