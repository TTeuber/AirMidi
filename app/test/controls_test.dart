// The control model against daisy_synth's real MIDI-CI resources (devices/daisy_synth/host writes them).
import 'package:airmidi/shared/synth/controls.dart';
import 'package:flutter_test/flutter_test.dart';

Map<String, dynamic> api() => {
      'info': {'manufacturer': 'Daisy', 'model': 'Poly Synth'},
      'controls': [
        {
          'title': 'Cutoff',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [74],
          'priority': 1,
          'default': 2164392960,
          'ctrlMapId': 'cutoff'
        },
        {
          'title': 'Wave',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [70],
          'priority': 1,
          'default': 0,
          'typeHint': 'valueSelect',
          'ctrlMapId': 'wave'
        },
        {
          'title': 'Glide',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [65],
          'priority': 1,
          'default': 0,
          'typeHint': 'toggle'
        },
        {
          'title': 'Detune',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [94],
          'priority': 2,
          'default': 1352663040
        },
        {'title': 'Bend', 'channel': 1, 'ctrlType': 'pBend', 'priority': 2, 'default': 2147483648},
        {'title': 'Pressure', 'channel': 2, 'ctrlType': 'polyPress', 'priority': 1}, // per note: not shown
        {
          'title': 'Hidden',
          'channel': 1,
          'ctrlType': 'cc',
          'ctrlIndex': [3],
          'priority': 5
        },
      ],
      'maps': {
        'cutoff': [
          {'value': 0, 'title': '20Hz'},
          {'value': 2147483648, 'title': '1k'},
          {'value': 4294967295, 'title': '20k'}
        ],
        'wave': [
          {'value': 0, 'title': 'Saw'},
          {'value': 1431655765, 'title': 'Square'},
          {'value': 2863311530, 'title': 'Triangle'}
        ],
      },
    };

void main() {
  test('reads the list: shown controls by priority, the rest counted', () {
    final d = Device.fromApi(api());
    expect(d.name, 'Daisy Poly Synth');
    expect(d.controls.map((c) => c.title), ['Cutoff', 'Wave', 'Glide', 'Detune', 'Bend']);
    expect(d.hidden, 2);
  });

  test('kinds: fader, a picker over named values, a toggle', () {
    final d = Device.fromApi(api());
    final k = {for (final c in d.controls) c.title: d.kind(c)};
    expect(
        k, {'Cutoff': Kind.fader, 'Wave': Kind.select, 'Glide': Kind.toggle, 'Detune': Kind.fader, 'Bend': Kind.fader});
    expect(d.options(d.controls[1]).map((o) => o.title), ['Saw', 'Square', 'Triangle']);
  });

  test('a fader shows its named point, or percent', () {
    final d = Device.fromApi(api());
    final cutoff = d.controls[0], detune = d.controls[3], bend = d.controls[4];
    expect(d.shown(cutoff), '1k'); // default just above the 1k point
    cutoff.value = 100;
    expect(d.shown(cutoff), '20Hz');
    expect(d.shown(detune), '31');
    expect(d.shown(bend), '0');
  });

  test('messages: 7-bit CC, 14-bit bend, on the control\'s channel', () {
    final d = Device.fromApi(api());
    expect(Device.message(d.controls[0]), [0xB0, 74, 64]);
    d.controls[1].value = d.options(d.controls[1])[2].value;
    expect(Device.message(d.controls[1]), [0xB0, 70, 85]); // 2863311530 >> 25
    expect(Device.message(d.controls[4]), [0xE0, 0, 64]);
  });

  test('the device\'s own CCs move the controls', () {
    final d = Device.fromApi(api());
    expect(d.receive([0xB0, 74, 127]), isTrue);
    expect(d.controls[0].value, kMax32);
    expect(d.receive([0xB1, 74, 0]), isFalse); // another channel
    expect(d.receive([0xB0, 70, 64]), isTrue);
    expect(d.options(d.controls[1])[d.nearestOption(d.controls[1])].title, 'Triangle');
  });

  test('a profile when there is no MIDI-CI list', () {
    final d = Device.fromApi({
      'profile': [
        {
          'title': 'Volume',
          'ctrlType': 'cc',
          'ctrlIndex': [7]
        }
      ]
    });
    expect(d.controls.single.title, 'Volume');
    expect(d.controls.single.channel, 0);
  });
}
