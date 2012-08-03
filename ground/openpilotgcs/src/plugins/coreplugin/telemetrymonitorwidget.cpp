#include "telemetrymonitorwidget.h"

#include <QObject>
#include <QtGui>
#include <QtGui/QFont>
#include <QDebug>

TelemetryMonitorWidget::TelemetryMonitorWidget(QWidget *parent) : QGraphicsView(parent)
{
    setMinimumSize(160,80);
    setMaximumSize(160,80);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameStyle(QFrame::NoFrame);
    setBackgroundBrush(Qt::transparent);

    QGraphicsScene *scene = new QGraphicsScene(0,0,160,80, this);
    scene->setBackgroundBrush(Qt::transparent);

    QSvgRenderer *renderer = new QSvgRenderer();
    if (renderer->load(QString(":/core/images/tx-rx.svg"))) {
        graph = new QGraphicsSvgItem();
        graph->setSharedRenderer(renderer);
        graph->setElementId("txrxBackground");

        QString name;
        QGraphicsSvgItem* pt;

        for (int i=0; i<NODE_NUMELEM; i++) {
            name = QString("tx%0").arg(i);
            if (renderer->elementExists(name)) {
                pt = new QGraphicsSvgItem();
                pt->setSharedRenderer(renderer);
                pt->setElementId(name);
                pt->setParentItem(graph);
                txNodes.append(pt);
            }

            name = QString("rx%0").arg(i);
            if (renderer->elementExists(name)) {
                pt = new QGraphicsSvgItem();
                pt->setSharedRenderer(renderer);
                pt->setElementId(name);
                pt->setParentItem(graph);
                rxNodes.append(pt);
            }
        }

        scene->addItem(graph);

        txSpeed = new QGraphicsTextItem();
        txSpeed->setDefaultTextColor(Qt::white);
        txSpeed->setFont(QFont("Helvetica",22,2));
        txSpeed->setParentItem(graph);
        scene->addItem(txSpeed);

        rxSpeed = new QGraphicsTextItem();
        rxSpeed->setDefaultTextColor(Qt::white);
        rxSpeed->setFont(QFont("Helvetica",22,2));
        rxSpeed->setParentItem(graph);
        scene->addItem(rxSpeed);

        scene->setSceneRect(graph->boundingRect());
        setScene(scene);
    }

    connected = false;
    txValue = 0.0;
    rxValue = 0.0;

    setMin(0.0);
    setMax(1200.0);

    showTelemetry();
}

TelemetryMonitorWidget::~TelemetryMonitorWidget()
{
    while (!txNodes.isEmpty())
        delete txNodes.takeFirst();

    while (!rxNodes.isEmpty())
        delete rxNodes.takeFirst();
}

void TelemetryMonitorWidget::connect()
{
    connected = true;

    //flash the lights
    updateTelemetry(maxValue, maxValue);
}

void TelemetryMonitorWidget::disconnect()
{
    //flash the lights
    updateTelemetry(maxValue, maxValue);

    connected = false;
    updateTelemetry(0.0,0.0);
}
/*!
  \brief Called by the UAVObject which got updated

  Updates the numeric value and/or the icon if the dial wants this.
  */
void TelemetryMonitorWidget::updateTelemetry(double txRate, double rxRate)
{
    txValue = txRate;
    rxValue = rxRate;

    showTelemetry();
}

// Converts the value into an percentage:
// this enables smooth movement in moveIndex below
void TelemetryMonitorWidget::showTelemetry()
{
    txIndex = (txValue-minValue)/(maxValue-minValue) * NODE_NUMELEM;
    rxIndex = (rxValue-minValue)/(maxValue-minValue) * NODE_NUMELEM;

    if (connected)
        this->setToolTip(QString("Tx: %0 bytes/sec\nRx: %1 bytes/sec").arg(txValue).arg(rxValue));
    else
        this->setToolTip(QString("Disconnected"));

    QGraphicsItem* txNode;
    QGraphicsItem* rxNode;

    for (int i=0; i < NODE_NUMELEM; i++) {
        txNode = txNodes.at(i);
        txNode->setPos((i*(txNode->boundingRect().width() + 8)) + 60, (txNode->boundingRect().height()/2) - 2);
        txNode->setVisible(connected && i < txIndex);
        txNode->update();

        rxNode = rxNodes.at(i);
        rxNode->setPos((i*(rxNode->boundingRect().width() + 8)) + 60, (rxNode->boundingRect().height()*2) - 2);
        rxNode->setVisible(connected && i < rxIndex);
        rxNode->update();
    }

    txSpeed->setPos(graph->boundingRect().right() - 100, txNodes.at(0)->pos().y() - 10);
    txSpeed->setPlainText(QString("%0").arg(txValue));

    rxSpeed->setPos(graph->boundingRect().right() - 100, rxNodes.at(0)->pos().y() - 10);
    rxSpeed->setPlainText(QString("%0").arg(rxValue));

    update();
}

void TelemetryMonitorWidget::showEvent(QShowEvent *event)
{
    Q_UNUSED(event);

    fitInView(graph, Qt::KeepAspectRatio);
}

void TelemetryMonitorWidget::resizeEvent(QResizeEvent* event)
{
    Q_UNUSED(event);

    graph->setPos(0,-100);
    fitInView(graph, Qt::KeepAspectRatio);
}

