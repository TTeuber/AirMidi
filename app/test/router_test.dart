import 'package:airmidi/desktop/router.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('route summaries: all, ranges, moved channels, transpose', () {
    final r = RouteSpec('keyboard', 'box');
    expect(r.summary, 'all');
    r.mask = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 9);
    expect(r.summary, '1-4 10');
    r.map[0] = 2;
    expect(r.summary, '1→3 2-4 10');
    r.transpose = -12;
    expect(r.summary, '1→3 2-4 10 -12');
    r.mask = 0;
    expect(r.summary, 'none');
  });

  test('routes save and load as JSON', () {
    final r = RouteSpec('a', 'b', mask: 0x0201, transpose: 5, lo: 36, hi: 72)..map[9] = 0;
    final back = RouteSpec.fromJson(r.toJson());
    expect([back.from, back.to, back.mask, back.map[9], back.transpose, back.lo, back.hi, back.types],
        ['a', 'b', 0x0201, 0, 5, 36, 72, r.types]);
  });
}
