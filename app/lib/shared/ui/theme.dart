// The house colours (forslike): background, text, grey, accent, and the strips' three hues, each a base and a
// lighter tone for what is moving.
import 'package:flutter/material.dart';

const cBg = Color(0xFF090C0F), cN = Color(0xFFA5B0B6), cGrey = Color(0xFF636363), cAc = Color(0xFFE9C944);
const stripHues = [
  (base: Color(0xFF5C8E67), drag: Color(0xFF86B990)), // fir
  (base: Color(0xFF5187AB), drag: Color(0xFF7BB2D9)), // slate
  (base: Color(0xFF96719E), drag: Color(0xFFC29BCB)), // heather
];
