// The settings panel (fgpanel.c) as its users see it: bridgeprobe.c (with the upscaling add-on) and overlay.c (without).
// FgpApply is what a pick in the panel calls. It was typed out in all three files: a field added in one compiled and
// called the wrong function in the others. bridgeprobe.c fills it by position, so new fields go at the end.
#pragma once
typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); void (*cap)(int choice); void (*preset)(int choice); void (*sharp)(int choice); void (*view)(int choice); void (*vsync)(int on); void (*fps)(int on); void (*tech)(int sr, int fg); void (*cull)(int choice); } FgpApply;
