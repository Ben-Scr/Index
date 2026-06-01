namespace Index.Components;

// Marker interface for blittable ECS component structs; implementors are auto-registered at script-load.
// Structs must be Sequential (non-sequential types are rejected by the registrar).
// Built-in C++ mirrors (Index.Native) carry an `internal const string NativeName` to bypass duplicate registration.
public interface IComponent { }
