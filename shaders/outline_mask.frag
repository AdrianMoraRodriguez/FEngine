#version 450

/// The selection mask: every pixel the selected object covers is written as 1.
layout(location = 0) out float outMask;

void main() {
    outMask = 1.0;
}
