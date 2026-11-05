#version 450
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 extra;
layout(location = 0) out vec4 color;
layout(push_constant) uniform Placement { vec4 offsetScale; } placement;
void main() {
    gl_Position = vec4(position.xy * placement.offsetScale.zw + placement.offsetScale.xy, position.z, 1.0);
    color = extra;
}
