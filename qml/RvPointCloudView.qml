pragma ComponentBehavior: Bound
import QtQuick
import QtQuick3D
import QtQuick3D.Helpers

Item {
    id: root
    required property var pointInstances
    required property var depthInstances
    required property var meshGeometry
    required property real depthPointDiameter
    required property var renderSettings

    readonly property vector3d defaultCameraTarget: Qt.vector3d(0, 0, -600)
    readonly property real defaultCameraDistance: 900
    readonly property real defaultOrbitYaw: -25
    readonly property real defaultOrbitPitch: -18
    property vector3d cameraTarget: defaultCameraTarget
    property real cameraDistance: defaultCameraDistance
    property real orbitYaw: defaultOrbitYaw
    property real orbitPitch: defaultOrbitPitch
    readonly property real gridStepMm: 100
    property bool meshVisible: false
    readonly property bool hasPoints: pointInstances.count > 0 || depthInstances.count > 0 || meshVisible
    readonly property var axisDirections: [Qt.vector3d(1, 0, 0), Qt.vector3d(0, -1, 0), Qt.vector3d(0, 0, -1)]
    readonly property var axisColors: ["#ce7979", "#7ca58a", "#7da7d2"]

    function resetView() {
        cameraTarget = defaultCameraTarget
        cameraDistance = defaultCameraDistance
        orbitYaw = defaultOrbitYaw
        orbitPitch = defaultOrbitPitch
    }

    View3D {
        id: view
        anchors.fill: parent
        camera: camera
        environment: SceneEnvironment {
            backgroundMode: SceneEnvironment.Color
            clearColor: "#161b22"
            antialiasingMode: root.renderSettings.lowCostRendering ? SceneEnvironment.NoAA : SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.Medium
        }

        Node {
            position: root.cameraTarget
            eulerRotation: Qt.vector3d(root.orbitPitch, root.orbitYaw, 0)
            PerspectiveCamera {
                id: camera
                z: root.cameraDistance
                clipNear: Math.max(1, root.cameraDistance / 2000)
                clipFar: Math.max(25000, root.cameraDistance * 4)
                fieldOfView: 45
                fieldOfViewOrientation: PerspectiveCamera.Vertical
                frustumCullingEnabled: false
            }
        }

        Model {
            objectName: "rvPointCloudPoints"
            visible: root.pointInstances.count > 0
            source: "#Cube"
            instancing: root.pointInstances
            castsShadows: false
            receivesShadows: false
            materials: CustomMaterial {
                shadingMode: CustomMaterial.Unshaded
                vertexShader: "RvCloudNativePoint.vert"
                fragmentShader: "RvCloudMesh.frag"
            }
        }

        Model {
            visible: root.depthInstances.count > 0
            source: "#Rectangle"
            instancing: root.depthInstances
            castsShadows: false
            receivesShadows: false
            materials: CustomMaterial {
                shadingMode: CustomMaterial.Unshaded
                cullMode: Material.NoCulling
                property vector2d diameterToClip: Qt.vector2d(
                    root.depthPointDiameter / Math.max(1, view.effectiveTextureSize.width || view.width),
                    root.depthPointDiameter / Math.max(1, view.effectiveTextureSize.height || view.height))
                vertexShader: "RvCloudPoint.vert"
                fragmentShader: "RvCloudPoint.frag"
            }
        }

        Model {
            objectName: "rvPointCloudMesh"
            visible: root.meshVisible
            geometry: root.meshGeometry
            castsShadows: false
            receivesShadows: false
            materials: CustomMaterial {
                shadingMode: CustomMaterial.Unshaded
                cullMode: Material.NoCulling
                vertexShader: "RvCloudMesh.vert"
                fragmentShader: "RvCloudMesh.frag"
            }
        }

        Model {
            visible: root.hasPoints
            x: Math.round(root.cameraTarget.x / root.gridStepMm) * root.gridStepMm
            z: Math.round(root.cameraTarget.z / root.gridStepMm) * root.gridStepMm
            eulerRotation.x: -90
            castsShadows: false
            geometry: GridGeometry {
                horizontalLines: 16
                verticalLines: 16
                horizontalStep: root.gridStepMm
                verticalStep: root.gridStepMm
            }
            materials: DefaultMaterial { lighting: DefaultMaterial.NoLighting; diffuseColor: "#303b48" }
        }

        Repeater3D {
            model: 3
            delegate: Model {
                id: axis
                required property int index
                visible: root.hasPoints
                source: "#Cube"
                castsShadows: false
                readonly property real axisLength: root.gridStepMm
                readonly property real thickness: axisLength * .014
                position: root.axisDirections[index].times(axisLength * .5)
                scale: Qt.vector3d(index === 0 ? axisLength : thickness,
                                   index === 1 ? axisLength : thickness,
                                   index === 2 ? axisLength : thickness).times(.01)
                materials: DefaultMaterial {
                    lighting: DefaultMaterial.NoLighting
                    diffuseColor: root.axisColors[axis.index]
                }
            }
        }
    }

    Repeater {
        model: 3
        delegate: Text {
            required property int index
            readonly property vector3d tip: root.axisDirections[index].times(root.gridStepMm * 1.14)
            readonly property vector3d screenPosition: project(tip, camera.sceneTransform, view.width, view.height)
            function project(position, transform, width, height) {
                return view.mapFrom3DScene(position)
            }
            text: ["X", "Y", "Z"][index]
            color: root.axisColors[index]
            font.bold: true
            x: screenPosition.x - width * .5
            y: screenPosition.y - height * .5
            visible: root.hasPoints && screenPosition.z > 0
                && x >= 0 && y >= 0 && x + width <= root.width && y + height <= root.height
        }
    }

    Text {
        visible: root.hasPoints
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 10
        color: "#a7b6c8"
        text: qsTr("Grid step %1 m").arg((root.gridStepMm / 1000).toLocaleString(Qt.locale(), "f", 2))
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        enabled: root.hasPoints
        property point previous
        onPressed: mouse => { previous = Qt.point(mouse.x, mouse.y) }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const dx = mouse.x - previous.x
            const dy = mouse.y - previous.y
            previous = Qt.point(mouse.x, mouse.y)
            if (mouse.buttons & Qt.MiddleButton) {
                const mmPerPixel = 2 * root.cameraDistance * Math.tan(camera.fieldOfView * Math.PI / 360)
                    / Math.max(1, view.height)
                root.cameraTarget = root.cameraTarget.plus(camera.right.times(-dx * mmPerPixel))
                    .plus(camera.up.times(dy * mmPerPixel))
            } else {
                root.orbitYaw -= dx * .45
                root.orbitPitch = Math.max(-85, Math.min(85, root.orbitPitch - dy * .45))
            }
        }
        onWheel: wheel => {
            const steps = wheel.angleDelta.y ? wheel.angleDelta.y / 120 : wheel.pixelDelta.y / 40
            if (!steps)
                return
            root.cameraDistance = Math.max(20, Math.min(60000, root.cameraDistance * Math.pow(.86, steps)))
        }
    }
}
