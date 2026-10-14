#include <mgif/scene_editor/StructuralDelta.h>

// StructuralDelta's private implementation is colocated with SceneCommands so
// it can retain TinyGLTF records without exposing that dependency in a public
// or internal header. This translation unit intentionally owns only the public
// header boundary and is wired into scene_editor_document by P4 integration.
