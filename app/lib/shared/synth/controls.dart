// The device behind the box, as its MIDI-CI control list describes it (M2-117 AllCtrlList + CtrlMapList, read by the
// box over TRS: GET /api/ci/device), or as a profile uploaded to the box. The same rules as the web page's play page
// (web/app.js): which controls to show, as what, the names of their values, and the MIDI 1.0 they send and follow.
// Values are MIDI-CI's 32-bit ones throughout.

const kMax32 = 0xFFFFFFFF;
const _types = {'cc', 'chPress', 'pBend'}; // per-note types need a note; RPN/NRPN are left for later

enum Kind { fader, select, toggle }

class MapEntry {
  final String title;
  final int value;
  const MapEntry(this.title, this.value);
}

class Control {
  final String title;
  final int channel; // 0-based
  final String type;
  final List<int> index;
  final int priority;
  final int def;
  final String? mapId, typeHint;
  final String path; // M2-117 paramPath ("/env/attack"), '' if the device sent none
  final int stepCount;
  final int lo, hi;
  int value;

  int order = 0; // position in the device's list: ties in priority keep it (Dart's sort is not stable)

  Control._(this.title, this.channel, this.type, this.index, this.priority, this.def, this.mapId, this.typeHint,
      this.path, this.stepCount, this.lo, this.hi)
      : value = def;

  static Control? fromJson(Map<String, dynamic> j) {
    final type = j['ctrlType'], title = j['title'];
    if (title is! String || title.isEmpty || !_types.contains(type)) return null;
    if ((j['priority'] ?? 3) >= 5 || j['recognize'] == 'none') return null;
    final mm = j['minMax'];
    final pBend = type == 'pBend';
    return Control._(
      title,
      (((j['channel'] ?? 1) as num).toInt() - 1) % 16,
      type,
      [for (final i in (j['ctrlIndex'] as List? ?? [])) (i as num).toInt()],
      ((j['priority'] ?? 3) as num).toInt(),
      ((j['default'] ?? (pBend ? 0x80000000 : 0)) as num).toInt(),
      j['ctrlMapId'] as String?,
      j['typeHint'] as String?,
      j['paramPath'] is String ? j['paramPath'] as String : '',
      ((j['stepCount'] ?? 0) as num).toInt(),
      mm is List && mm.length == 2 ? (mm[0] as num).toInt() : 0,
      mm is List && mm.length == 2 ? (mm[1] as num).toInt() : kMax32,
    );
  }
}

class Device {
  final String name;
  final List<Control> controls;
  final Map<String, List<MapEntry>> maps;
  final int hidden; // controls in the list this screen cannot show
  Device(this.name, this.controls, this.maps, this.hidden);

  /// From /api/ci/device: {"info", "controls", "maps", "profile"}; a profile is a bare control list or the same
  /// shape. MIDI-CI's list wins over a profile, as on the web page.
  factory Device.fromApi(Map<String, dynamic> d) {
    final profile = d['profile'] is List ? {'controls': d['profile']} : (d['profile'] as Map<String, dynamic>? ?? {});
    final fromCi = d['controls'] is List;
    final info = ((fromCi ? d['info'] : profile['info']) ?? d['info']) as Map<String, dynamic>?;
    final all = ((fromCi ? d['controls'] : profile['controls']) as List?) ?? [];
    final rawMaps = ((fromCi ? d['maps'] : profile['maps']) as Map<String, dynamic>?) ?? {};
    final maps = {
      for (final e in rawMaps.entries)
        if (e.value is List)
          e.key: [
            for (final o in e.value as List)
              if (o is Map && o['title'] is String && o['value'] is num)
                MapEntry(o['title'], (o['value'] as num).toInt())
          ]
    };
    final controls = [
      for (final c in all)
        if (c is Map<String, dynamic>) Control.fromJson(c)
    ].whereType<Control>().toList();
    for (var i = 0; i < controls.length; i++) {
      controls[i].order = i;
    }
    controls.sort((a, b) => a.priority != b.priority ? a.priority.compareTo(b.priority) : a.order.compareTo(b.order));
    final name = info == null
        ? ''
        : [info['manufacturer'], info['model']].whereType<String>().where((s) => s.isNotEmpty).join(' ');
    return Device(name, controls, maps, all.length - controls.length);
  }

  Kind kind(Control c) {
    if (c.typeHint == 'toggle' || c.typeHint == 'momentary') return Kind.toggle;
    if (maps[c.mapId] != null && c.typeHint == 'valueSelect') return Kind.select;
    if (c.stepCount > 1 && c.stepCount <= 8) return Kind.select;
    return Kind.fader;
  }

  /// A select's choices: the named values, or n evenly spaced steps (k * 2^32 / n).
  List<MapEntry> options(Control c) {
    final m = maps[c.mapId];
    if (m != null && kind(c) == Kind.select) return m;
    return [for (var k = 0; k < c.stepCount; k++) MapEntry('${k + 1}', (k * 4294967296 / c.stepCount).floor())];
  }

  /// The select option nearest to the value (a value from the device need not be one of the listed ones).
  int nearestOption(Control c) {
    final o = options(c);
    var best = 0;
    for (var j = 1; j < o.length; j++) {
      if ((o[j].value - c.value).abs() < (o[best].value - c.value).abs()) best = j;
    }
    return best;
  }

  /// What a fader shows: the named value at or below it (a cutoff map names points along the range), else percent.
  String shown(Control c) {
    final m = maps[c.mapId];
    if (m != null && m.isNotEmpty) {
      final below = m.where((o) => o.value <= c.value).toList()..sort((a, b) => b.value.compareTo(a.value));
      if (below.isNotEmpty) return below.first.title;
    }
    if (c.type == 'pBend') return '${((c.value - 0x80000000) / 0x80000000 * 100).round()}';
    return '${((c.value - c.lo) / ((c.hi - c.lo) == 0 ? 1 : c.hi - c.lo) * 100).round()}';
  }

  double fraction(Control c) => ((c.value - c.lo) / ((c.hi - c.lo) == 0 ? 1 : c.hi - c.lo)).clamp(0.0, 1.0);

  /// The MIDI 1.0 message for a control's value (the box's links carry MIDI 1.0).
  static List<int> message(Control c) {
    final v = c.value.clamp(0, kMax32);
    switch (c.type) {
      case 'cc':
        return [0xB0 | c.channel, c.index.isEmpty ? 0 : c.index[0] & 0x7F, v >> 25];
      case 'chPress':
        return [0xD0 | c.channel, v >> 25];
      default: // pBend
        final b = v >> 18;
        return [0xE0 | c.channel, b & 0x7F, b >> 7];
    }
  }

  /// A message from the device (its own knobs moved): the controls it sets follow. True if any changed.
  bool receive(List<int> m) {
    if (m.isEmpty) return false;
    final ch = m[0] & 0x0F, t = m[0] & 0xF0;
    var changed = false;
    for (final c in controls) {
      if (c.channel != ch) continue;
      int? v;
      if (c.type == 'cc' && t == 0xB0 && m.length > 2 && c.index.isNotEmpty && m[1] == c.index[0]) {
        v = (m[2] / 127 * kMax32).round();
      } else if (c.type == 'chPress' && t == 0xD0 && m.length > 1) {
        v = (m[1] / 127 * kMax32).round();
      } else if (c.type == 'pBend' && t == 0xE0 && m.length > 2) {
        v = ((m[1] | m[2] << 7) / 16383 * kMax32).round();
      }
      if (v != null && v != c.value) {
        c.value = v;
        changed = true;
      }
    }
    return changed;
  }
}
