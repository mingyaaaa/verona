

module {
  func @f(%arg0: none, %arg1: none) -> none {
    %0 = "verona.alloca"() : () -> !type.alloca
    %1 = "verona.store"(%arg0, %0) : (none, !type.alloca) -> !type.unk
    %2 = "verona.alloca"() : () -> !type.alloca
    %3 = "verona.store"(%arg1, %2) : (none, !type.alloca) -> !type.unk
    verona.while  {
      %6 = "verona.load"(%0) : (!type.alloca) -> !type.unk
      %7 = "verona.constant(5)"() : () -> !type.int
      %8 = "verona.lt"(%6, %7) : (!type.unk, !type.int) -> i1
      verona.loop_exit %8 : i1
      %9 = "verona.load"(%0) : (!type.alloca) -> !type.unk
      %10 = "verona.load"(%2) : (!type.alloca) -> !type.unk
      %11 = "verona.ne"(%9, %10) : (!type.unk, !type.unk) -> i1
      cond_br %11, ^bb1, ^bb2
    ^bb1:  // pred: ^bb0
      %12 = "verona.load"(%0) : (!type.alloca) -> !type.unk
      %13 = "verona.constant(1)"() : () -> !type.int
      %14 = "verona.add"(%12, %13) : (!type.unk, !type.int) -> !type.unk
      %15 = "verona.store"(%14, %0) : (!type.unk, !type.alloca) -> !type.unk
      br ^bb2
    ^bb2:  // 2 preds: ^bb0, ^bb1
      verona.continue
    }
    %4 = "verona.load"(%0) : (!type.alloca) -> !type.unk
    %5 = "verona.cast"(%4) : (!type.unk) -> none
    return %5 : none
  }
}