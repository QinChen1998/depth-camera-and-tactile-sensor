pragma ComponentBehavior: Bound
import QtQuick
import QtQuick3D
Item {
    id: root
    required property var display
    // Copy the property-backed sequence once per frame; all delegates and
    // inspection share this snapshot instead of repeatedly crossing into C++.
    readonly property var pointSnapshot: {
        const values = display.points
        const result = []
        for (let i = 0; i < values.length; ++i) result.push(values[i])
        return result
    }
    readonly property int rawSnapshotCount: {
        let count = 0
        while (count < pointSnapshot.length && !pointSnapshot[count].interpolated) ++count
        return count
    }
    readonly property var rawProjections: {
        const result = []
        for (let i = 0; i < rawSnapshotCount; ++i) result.push(project(displayPosition(pointSnapshot[i])))
        return result
    }
    property real orbitYaw: -35
    property real orbitPitch: -25
    property real cameraDistance: fittedDistance
    readonly property real fittedDistance: Math.max(display.surface.size.y * 1000, display.surface.size.x * 1000 / Math.max(.35, width / Math.max(1, height))) * 1.5 / (2 * Math.tan(Math.PI / 8)) + display.surface.size.z * 1000
    readonly property vector3d modelCenter: sensorRoot.mapPositionToScene(display.surface.center)
    readonly property string sensorType: display.surface.id
    property vector3d cameraPan: modelCenter
    property int inspectedPoint: -1
    readonly property real rawPointPickRadius: 12
    property bool autoFrameDots: true
    property real framedDotGain: -1
    readonly property bool dotMatrixMode: display.dotMatrix
    function fitDotMatrix(expandOnly) {
        if (!display.dotMatrix || !display.valid) return
        const halfSize = display.surface.size.times(500)
        let low = modelCenter.minus(halfSize)
        let high = modelCenter.plus(halfSize)
        // Native bounds already include every raw/interpolated lifted dot.
        // Transform eight corners, independent of interpolation density.
        const minimum = display.dotBoundsMinimum
        const maximum = display.dotBoundsMaximum
        for (let i = 0; i < 8; ++i) {
            const p = sensorRoot.mapPositionToScene(Qt.vector3d(i & 1 ? maximum.x : minimum.x,
                i & 2 ? maximum.y : minimum.y, i & 4 ? maximum.z : minimum.z))
            low = Qt.vector3d(Math.min(low.x, p.x - .35), Math.min(low.y, p.y - .35), Math.min(low.z, p.z - .35))
            high = Qt.vector3d(Math.max(high.x, p.x + .35), Math.max(high.y, p.y + .35), Math.max(high.z, p.z + .35))
        }
        const center = expandOnly ? cameraPan : low.plus(high).times(.5)
        const radius = Math.hypot(Math.max(Math.abs(low.x - center.x), Math.abs(high.x - center.x)),
                                  Math.max(Math.abs(low.y - center.y), Math.abs(high.y - center.y)),
                                  Math.max(Math.abs(low.z - center.z), Math.abs(high.z - center.z)))
        const halfAngle = Math.atan(Math.tan(Math.PI / 8) * Math.min(1, Math.max(.1, width / Math.max(1, height))))
        const distance = Math.max(20, radius / Math.sin(halfAngle) * 1.08)
        if (!expandOnly) cameraPan = center
        if (!expandOnly || distance > cameraDistance) cameraDistance = distance
        framedDotGain = display.arrowGain
    }
    function resetView() { orbitYaw = -35; orbitPitch = -25; cameraDistance = fittedDistance; cameraPan = modelCenter; autoFrameDots = true; fitDotMatrix(false) }
    function frontView() { orbitYaw = 0; orbitPitch = 0; cameraDistance = fittedDistance; cameraPan = modelCenter; autoFrameDots = true; fitDotMatrix(false) }
    onSensorTypeChanged: { cameraDistance = fittedDistance; cameraPan = modelCenter; inspectedPoint = -1; autoFrameDots = true; fitDotMatrix(false) }
    onDotMatrixModeChanged: { autoFrameDots = true; if (dotMatrixMode) fitDotMatrix(false); else { cameraDistance = fittedDistance; cameraPan = modelCenter } }
    onWidthChanged: Qt.callLater(function() { if (root.autoFrameDots) root.fitDotMatrix(true) })
    onHeightChanged: Qt.callLater(function() { if (root.autoFrameDots) root.fitDotMatrix(true) })
    Connections {
        target: root.display
        function onFrameChanged() {
            if (!root.display.dotMatrix) return
            if (root.framedDotGain !== root.display.arrowGain) { root.autoFrameDots = true; root.fitDotMatrix(false) }
            else if (root.autoFrameDots) root.fitDotMatrix(true)
        }
    }
    function panView(dx, dy) {
        const units = 2 * cameraDistance * Math.tan(Math.PI / 8) / Math.max(1, view.height)
        const target = cameraPan.plus(camera.right.times(-dx * units)).plus(camera.up.times(dy * units))
        cameraPan = Qt.vector3d(Math.max(-80, Math.min(80, target.x)), Math.max(-80, Math.min(80, target.y)), Math.max(-60, Math.min(60, target.z)))
    }
    function project(position) {
        if (!isFinite(orbitYaw + orbitPitch + cameraDistance + cameraPan.x + cameraPan.y + cameraPan.z + view.width + view.height)) return Qt.vector3d(-1000, -1000, -1)
        return view.mapFrom3DScene(sensorRoot.mapPositionToScene(position))
    }
    function displayPosition(datum) { return display.dotMatrix ? datum.dotPosition : datum.position }
    function pickPoint(x, y) {
        if (!display.valid || !(x >= 0 && x < width && y >= 0 && y < height)) return -1
        const hit = view.pick(x, y)
        if (hit.objectHit && hit.objectHit.objectName === "tactileInterpolatedPoints")
            return rawSnapshotCount + hit.instanceIndex
        if (hit.objectHit && hit.objectHit.pointIndex !== undefined)
            return hit.objectHit.pointIndex
        // Enlarge the target in logical screen pixels, independent of zoom
        // and glyph size. Exact geometry hits keep their inspection identity.
        let nearest = -1
        let nearestDistance = rawPointPickRadius * rawPointPickRadius
        let nearestDepth = Infinity
        for (let i = 0; i < rawSnapshotCount; ++i) {
            const p = rawProjections[i]
            if (!(p.z > 0 && p.x >= 0 && p.x < width && p.y >= 0 && p.y < height)) continue
            const distance = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y)
            if (distance < nearestDistance || (distance === nearestDistance && p.z < nearestDepth)) {
                nearest = i
                nearestDistance = distance
                nearestDepth = p.z
            }
        }
        return nearest
    }
    function labelFits(index, position, labelWidth, labelHeight) {
        if (!display.valid || position.z <= 0 || position.x - labelWidth / 2 < 2 || position.x + labelWidth / 2 > width - 2 || position.y < 0 || position.y + 11 + labelHeight > height - 25) return false
        for (let earlier = 0; earlier < index; ++earlier) {
            if (pointSnapshot[earlier].interpolated) continue
            const other = rawProjections[earlier]
            if (Math.abs(position.x - other.x) < labelWidth + 3 && Math.abs(position.y - other.y) < labelHeight + 3) return false
        }
        return true
    }
    View3D {
        id: view
        objectName: "tactileView3d"
        anchors.fill: parent
        camera: camera
        environment: SceneEnvironment {
            objectName: "tactileEnvironment"
            backgroundMode: SceneEnvironment.Color
            clearColor: root.display.background
            antialiasingMode: root.display.lowCostRendering ? SceneEnvironment.NoAA : SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.Medium
        }
        Node {
            position: root.cameraPan
            eulerRotation: Qt.vector3d(root.orbitPitch, root.orbitYaw, 0)
            PerspectiveCamera { id: camera; z: root.cameraDistance; clipNear: 1; clipFar: Math.max(1500, root.cameraDistance * 3); fieldOfView: 45; fieldOfViewOrientation: PerspectiveCamera.Vertical }
        }
        // Same key/fill direction, intensity and cool fill as HandView.
        DirectionalLight { objectName: "tactileKeyLight"; eulerRotation: Qt.vector3d(-35, -25, 0); brightness: 1.1; castsShadow: !root.display.lowCostRendering }
        DirectionalLight { eulerRotation: Qt.vector3d(40, 155, 0); brightness: 0.45; color: "#9bbcff" }
        Node {
            id: sensorRoot
            objectName: "tactileSensorRoot"
            // URDF/STL and projected anchors remain in metres. Convert once here.
            scale: Qt.vector3d(1000, 1000, 1000)
            Model {
                objectName: "tactileSensorModel"
                opacity: root.display.modelOpacity
                visible: root.display.surface.valid
                source: root.display.surface.mesh
                position: root.display.surface.position
                rotation: root.display.surface.rotation
                pickable: false
                materials: PrincipledMaterial { baseColor: "#77818c"; roughness: 0.65; metalness: 0.08 }
            }
        Repeater3D {
            model: root.rawSnapshotCount
            delegate: Node {
                id: pointNode
                required property int index
                property var datum: root.pointSnapshot[index]
                position: root.displayPosition(datum)
                visible: root.display.valid
                Node {
                    // Fixed display glyphs: one old glyph unit is 0.1 mm.
                    scale: Qt.vector3d(0.0001, 0.0001, 0.0001)
                Model {
                    objectName: "tactilePoint" + pointNode.index
                    property int pointIndex: pointNode.index
                    source: "#Sphere"
                    pickable: true
                    property real radius: root.display.dotMatrix ? 3.5 : pointNode.datum.radius
                    scale: Qt.vector3d(radius / 50, radius / 50, radius / 50)
                    materials: PrincipledMaterial { baseColor: pointNode.datum.color; roughness: 0.32; metalness: 0.08; opacity: pointNode.datum.selected ? 1 : 0.65 }
                    castsShadows: !root.display.lowCostRendering
                }
                TactileArrow {
                    objectName: "pointArrow" + pointNode.index
                    position: pointNode.datum.offset
                    visible: !root.display.dotMatrix && root.display.pointArrows && pointNode.datum.arrowVisible
                    rotation: pointNode.datum.rotation
                    forceLength: pointNode.datum.length
                    thickness: 0.75 * root.display.arrowWidthGain
                    shaftRadius: pointNode.datum.radius / 2 * root.display.arrowWidthGain
                    forceColor: root.display.textColor
                }
                Repeater3D {
                    model: 3
                    delegate: TactileArrow {
                        required property int index
                        readonly property var component: pointNode.datum.components[index]
                        objectName: "componentArrow" + pointNode.index + "_" + index
                        visible: !root.display.dotMatrix && root.display.componentArrows && component.arrowVisible
                        position: component.offset
                        rotation: component.rotation
                        forceLength: component.length
                        thickness: 1.4 * root.display.arrowWidthGain
                        shaftRadius: 0.875 * root.display.arrowWidthGain
                        forceColor: ["#ff525d", "#48e58b", "#40baff"][index]
                    }
                }
            }
        }
        }
        // Interpolated glyphs share five meshes and native instance buffers.
        // Raw points retain their existing nodes and inspection identities.
        Model {
            objectName: "tactileInterpolatedPoints"
            source: "#Sphere"
            instancing: root.display.interpolatedPoints
            visible: root.display.valid && root.display.interpolatedPoints.count > 0
            pickable: true
            materials: PrincipledMaterial { baseColor: "white"; roughness: 0.32; metalness: 0.08 }
            castsShadows: !root.display.lowCostRendering
        }
        Model {
            objectName: "tactileInterpolatedPointShafts"
            source: "#Cylinder"
            instancing: root.display.interpolatedPointShafts
            visible: root.display.valid && !root.display.dotMatrix && root.display.pointArrows && root.display.interpolatedPointShafts.count > 0
            onBoundsChanged: root.display.setArrowMeshBounds(false, bounds.minimum.y, bounds.maximum.y - bounds.minimum.y)
            materials: PrincipledMaterial { baseColor: "white"; roughness: 0.42; metalness: 0.08 }
            castsShadows: false
        }
        Model {
            objectName: "tactileInterpolatedPointHeads"
            source: "#Cone"
            instancing: root.display.interpolatedPointHeads
            visible: root.display.valid && !root.display.dotMatrix && root.display.pointArrows && root.display.interpolatedPointHeads.count > 0
            onBoundsChanged: root.display.setArrowMeshBounds(true, bounds.minimum.y, bounds.maximum.y - bounds.minimum.y)
            materials: PrincipledMaterial { baseColor: "white"; roughness: 0.42; metalness: 0.08 }
            castsShadows: false
        }
        Model {
            objectName: "tactileInterpolatedComponentShafts"
            source: "#Cylinder"
            instancing: root.display.interpolatedComponentShafts
            visible: root.display.valid && !root.display.dotMatrix && root.display.componentArrows && root.display.interpolatedComponentShafts.count > 0
            onBoundsChanged: root.display.setArrowMeshBounds(false, bounds.minimum.y, bounds.maximum.y - bounds.minimum.y)
            materials: PrincipledMaterial { baseColor: "white"; roughness: 0.42; metalness: 0.08 }
            castsShadows: false
        }
        Model {
            objectName: "tactileInterpolatedComponentHeads"
            source: "#Cone"
            instancing: root.display.interpolatedComponentHeads
            visible: root.display.valid && !root.display.dotMatrix && root.display.componentArrows && root.display.interpolatedComponentHeads.count > 0
            onBoundsChanged: root.display.setArrowMeshBounds(true, bounds.minimum.y, bounds.maximum.y - bounds.minimum.y)
            materials: PrincipledMaterial { baseColor: "white"; roughness: 0.42; metalness: 0.08 }
            castsShadows: false
        }
        TactileArrow {
            objectName: "resultantArrow"
            readonly property real modelScale: 0.0001 * root.display.resultantSizeGain
            scale: Qt.vector3d(modelScale, modelScale, modelScale)
            position: root.display.resultant.position
            rotation: root.display.resultant.rotation
            forceLength: root.display.resultant.length
            thickness: 2 * root.display.arrowWidthGain
            shaftRadius: root.pointSnapshot[0].radius / 2 * root.display.arrowWidthGain
            forceColor: root.display.accent
            visible: root.display.valid && !root.display.dotMatrix && root.display.resultantArrow && root.display.resultant.arrowVisible
        }
        }
        Node {
            position: Qt.vector3d(-18, -18, 0)
            scale: Qt.vector3d(0.1, 0.1, 0.1)
            TactileArrow { forceLength: 30; thickness: 0.65; eulerRotation.z: -90; forceColor: "#a44242" }
            TactileArrow { forceLength: 30; thickness: 0.65; forceColor: "#397d4c" }
            TactileArrow { forceLength: 30; thickness: 0.65; eulerRotation.x: -90; forceColor: "#3669a8" }
        }
    }
    Repeater {
        model: 3
        delegate: Text {
            required property int index
            property vector3d projected: root.project(index === 0 ? Qt.vector3d(-.0147, -.018, 0) : index === 1 ? Qt.vector3d(-.018, -.0147, 0) : Qt.vector3d(-.018, -.018, -.0033))
            x: projected.x; y: projected.y
            text: ["X", "Y", "Z"][index]; font.pixelSize: 11; color: root.display.textColor
            visible: projected.z > 0 && x > 0 && x < root.width - width && y > 0 && y < root.height - height
        }
    }
    MouseArea {
        anchors.fill: parent
        objectName: "tactilePointerArea"
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        hoverEnabled: false
        property real previousX
        property real previousY
        property real pressX
        property real pressY
        property int dragButton: Qt.NoButton
        property bool dragged: false
        onPressed: mouse => { forceActiveFocus(); dragButton = mouse.button; pressX = previousX = mouse.x; pressY = previousY = mouse.y; dragged = false }
        onPositionChanged: mouse => {
            if (dragButton !== Qt.NoButton && (mouse.buttons & dragButton)) {
                if (Math.hypot(mouse.x - pressX, mouse.y - pressY) >= 6) dragged = true
                if (dragged) {
                    root.autoFrameDots = false
                    if (dragButton === Qt.MiddleButton) root.panView(mouse.x - previousX, mouse.y - previousY)
                    else { root.orbitYaw -= (mouse.x - previousX) * 0.45; root.orbitPitch = Math.max(-85, Math.min(85, root.orbitPitch - (mouse.y - previousY) * 0.45)) }
                    root.inspectedPoint = -1
                }
                previousX = mouse.x; previousY = mouse.y
            }
        }
        onReleased: mouse => {
            if (mouse.button !== dragButton) return
            if (!dragged && dragButton === Qt.LeftButton) {
                root.inspectedPoint = root.pickPoint(mouse.x, mouse.y)
                if (root.inspectedPoint >= 0 && root.inspectedPoint < root.rawSnapshotCount)
                    root.display.selectRawPoint(root.inspectedPoint)
            }
            dragButton = Qt.NoButton
        }
        onCanceled: { dragButton = Qt.NoButton; dragged = false }
        onWheel: wheel => { root.autoFrameDots = false; root.cameraDistance = Math.max(20, Math.min(root.display.dotMatrix ? 5000 : 250, root.cameraDistance * (wheel.angleDelta.y > 0 ? 0.88 : 1.14))); wheel.accepted = true }
    }
    Rectangle {
        anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 5
        width: Math.min(root.width - 10, details.implicitWidth + 12); height: details.implicitHeight + 10
        color: root.display.background; border.color: root.display.border
        visible: root.inspectedPoint >= 0 && root.display.valid
        Text {
            id: details
            anchors.centerIn: parent
            property var p: root.pointSnapshot[Math.max(0, Math.min(root.inspectedPoint, root.pointSnapshot.length - 1))]
            text: p.id + "\n" + (p.interpolated ? qsTr("Interpolated counts") : (root.display.sourceLabel.indexOf("DELTA") >= 0 ? qsTr("Delta counts") : qsTr("Raw counts"))) + ": X " + p.fx.toFixed(0) + "  Y " + p.fy.toFixed(0) + "  Z " + p.fz.toFixed(0) + "\nXYZ: " + p.components[0].value + " / " + p.components[1].value + " / " + p.components[2].value
            color: root.display.textColor; font.pixelSize: 12
        }
    }
    Text { objectName: "tactileNoDataMessage"; anchors.centerIn: parent; visible: !root.display.valid; text: qsTr("Device disconnected / data unavailable"); color: root.display.textColor }
}
