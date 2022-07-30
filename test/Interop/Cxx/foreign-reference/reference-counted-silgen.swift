// RUN: %target-swift-emit-silgen %s -I %S/Inputs -enable-experimental-cxx-interop | %FileCheck %s
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import POD
import ReferenceCounted

// CHECK-LABEL: sil [ossa] @$s4main11testTrivialyyF : $@convention(thin) () -> ()
// CHECK-NOT: retain
// CHECK-NOT: release
// CHECK-NOT: copy_value
// CHECK-NOT: destroy_value
// CHECK-NOT: begin_borrow
// CHECK-NOT: end_borrow
// CHECK: return
// CHECK-LABEL: end sil function '$s4main11testTrivialyyF'
public func testTrivial() {
    let x = Empty.create()
    let t = (x, x, x)
}

// CHECK-LABEL: sil [ossa] @$s4main14testNonTrivialyyF : $@convention(thin) () -> ()
// CHECK:  copy_value %{{[0-9]+}} : $LocalCount
// CHECK:  copy_value %{{[0-9]+}} : $LocalCount
// CHECK:  copy_value %{{[0-9]+}} : $LocalCount
// CHECK: return
// CHECK-LABEL: end sil function '$s4main14testNonTrivialyyF'
public func testNonTrivial() {
    let x = LocalCount.create()
    let t = (x, x, x)
}
