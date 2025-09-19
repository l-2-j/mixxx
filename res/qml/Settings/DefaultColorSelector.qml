import QtQuick
import QtQuick.Layouts
import Mixxx 1.0 as Mixxx

RowLayout {
    id: root

    // FIXME remove hardcoded width
    readonly property double cellSize: Math.min(20, Math.max(10, (240 - root.padding * 2) / root.palette.length))
    property int currentIndex: 0
    property double padding: 10
    required property var palette

    spacing: 3

    Repeater {
        model: root.palette

        Item {
            required property int index
            required property color modelData

            height: width
            width: root.cellSize - root.spacing

            Rectangle {
                anchors.centerIn: parent
                color: modelData
                height: root.currentIndex == index ? parent.height : parent.height / 2
                radius: 3
                width: root.currentIndex == index ? parent.width : parent.width / 2
            }
            MouseArea {
                anchors.fill: parent

                onPressed: {
                    root.currentIndex = index;
                }
            }
        }
    }
}
