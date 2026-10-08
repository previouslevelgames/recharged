#pragma once
namespace mscharged::platform {
// A numeric native view of the exact original completed ARC file. Offsets and
// byte records keep their original locations; the raw resource stays unchanged.
// The caller must retain the original allocation through every borrowed use.
const void* NativeHBMLayoutHeader(const void* originalResource);
}
