pragma ComponentBehavior: Bound
import QtQuick
import QtQuick3D
Node {
    id: arrow
    property real forceLength: 0
    property real thickness: 1
    property color forceColor: "steelblue"
    // Force controls shaft length only. Display thickness scales shaft and
    // head widths independently of length; the head height stays fixed.
    readonly property real shaftLength: isFinite(forceLength) ? Math.max(0, forceLength) : 0
    property real shaftRadius: thickness * 0.4
    readonly property real headLength: 3
    readonly property real headRadius: thickness * 2.7 * 0.3
    Model {
        id: shaft
        objectName: "arrowShaft"
        source: "#Cylinder"
        visible: arrow.shaftLength > 0
        // Qt's cylinder spans Y=-50..50; its tail stays at the force anchor.
        // Use loaded bounds to account for the built-in mesh's quantization.
        readonly property real nativeHeight: bounds.maximum.y > bounds.minimum.y ? bounds.maximum.y - bounds.minimum.y : 100
        readonly property real nativeBottom: bounds.maximum.y > bounds.minimum.y ? bounds.minimum.y : -50
        y: -nativeBottom * arrow.shaftLength / nativeHeight
        scale: Qt.vector3d(arrow.shaftRadius / 50, arrow.shaftLength / shaft.nativeHeight, arrow.shaftRadius / 50)
        materials: PrincipledMaterial { baseColor: arrow.forceColor; roughness: 0.42; metalness: 0.08 }
        castsShadows: false
    }
    Model {
        id: head
        objectName: "arrowHead"
        source: "#Cone"
        visible: arrow.shaftLength > 0
        // Qt 6.11's cone spans Y=0..100 (base at origin), unlike Cylinder.
        // Place that base exactly at the shaft end, with no force-based scaling.
        readonly property real nativeHeight: bounds.maximum.y > bounds.minimum.y ? bounds.maximum.y - bounds.minimum.y : 100
        readonly property real nativeBottom: bounds.maximum.y > bounds.minimum.y ? bounds.minimum.y : 0
        y: arrow.shaftLength - nativeBottom * arrow.headLength / nativeHeight
        scale: Qt.vector3d(arrow.headRadius / 50, arrow.headLength / head.nativeHeight, arrow.headRadius / 50)
        materials: PrincipledMaterial { baseColor: arrow.forceColor; roughness: 0.42; metalness: 0.08 }
        castsShadows: false
    }
}
