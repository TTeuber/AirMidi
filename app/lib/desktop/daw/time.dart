// Beats and engine time (ns): the timeline the player plans on, and what it hands to the engine.

/// Beats <-> engine time at the current tempo, from an anchor (where the last tempo change happened).
class Timeline {
  int anchorNs;
  double anchorBeat;
  double bpm;
  Timeline(this.anchorNs, this.bpm, {this.anchorBeat = 0});

  double beatAt(int ns) => anchorBeat + (ns - anchorNs) * bpm / 60e9;
  int nsAt(double beat) => anchorNs + ((beat - anchorBeat) * 60e9 / bpm).round();

  /// A new tempo from [nowNs] on; the beats before it keep their times.
  void setBpm(double b, int nowNs) {
    anchorBeat = beatAt(nowNs);
    anchorNs = nowNs;
    bpm = b;
  }
}

class SeqEvent {
  final int tNs;
  final String port;
  final List<int> bytes;
  const SeqEvent(this.tNs, this.port, this.bytes);

  @override
  String toString() => '$tNs $port $bytes';
}

/// A note that was scheduled: what has to be ended if play stops before its note off goes out.
class ScheduledNote {
  final String port;
  final int channel, note, offNs;
  const ScheduledNote(this.port, this.channel, this.note, this.offNs);
}

/// By time; at the same time note offs first (a note ending just as the next starts), then everything else (an MPE
/// note's starting bend), then note ons; otherwise in the order made (Dart's sort is not stable).
void sortEvents(List<SeqEvent> out) {
  final order = {for (var i = 0; i < out.length; i++) out[i]: i};
  int rank(SeqEvent e) {
    final t = e.bytes[0] & 0xF0;
    if (t == 0x80 || (t == 0x90 && e.bytes.length > 2 && e.bytes[2] == 0)) return 0;
    return t == 0x90 ? 2 : 1;
  }

  out.sort((a, b) => a.tNs != b.tNs
      ? a.tNs.compareTo(b.tNs)
      : rank(a) != rank(b)
          ? rank(a).compareTo(rank(b))
          : order[a]!.compareTo(order[b]!));
}
