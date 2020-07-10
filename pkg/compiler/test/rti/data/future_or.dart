// Copyright (c) 2018, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// @dart = 2.7

import 'dart:async';

/*spec.class: global#Future:implicit=[Future<A*>],indirect,needsArgs*/

/*class: A:explicit=[FutureOr<A*>*],implicit=[A,Future<A*>]*/
class A {}

main() {
  new A() is FutureOr<A>;
}
