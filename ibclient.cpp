#include "ibclient.h"
#include "ibdefines.h"
#include "iborder.h"
#include "ibcontract.h"
#include "iborderstate.h"
#include "ibexecution.h"
#include "ibbardata.h"
#include "ibscandata.h"
#include "ibcommissionreport.h"
#include "ibsocketerrors.h"
#include "ibtagvalue.h"

#include <QTcpSocket>
#include <QDebug>

#include <cfloat>


static const qint64 BUFFER_SIZE_HIGH_MARK = 1 * 1024 * 1024; // 1 MB


IBClient::IBClient(QObject *parent)
    : QObject(parent)
    , m_socket(NULL)
    , m_clientId(0)
    , m_outBuffer(QByteArray())
    , m_inBuffer(QByteArray())
    , m_connected(false)
    , m_serverVersion(0)
    , m_twsTime(QByteArray())
    , m_begIdx(0)
    , m_endIdx(0)
    , m_extraAuth(0)
{
    m_socket = new QTcpSocket(this);

    connect(m_socket, SIGNAL(connected()),
            this, SLOT(onConnected()));
    connect(m_socket, SIGNAL(readyRead()),
            this, SLOT(onReadyRead()));
}

IBClient::~IBClient() {}



void IBClient::connectToTWS(const QString &host, quint16 port, int clientId)
{
    Q_UNUSED(host);

    m_clientId = clientId;
    m_socket->connectToHost(QString("127.0.0.1"), port);
    encodeField(CLIENT_VERSION);
    send();
}

void IBClient::disconnectTWS()
{
    m_twsTime.clear();
    m_serverVersion = 0;
    m_connected = false;
    m_extraAuth = false;
    m_clientId = -1;
    m_outBuffer.clear();
    m_inBuffer.clear();

    m_socket->disconnectFromHost();
}


void IBClient::send()
{
    int sent = m_socket->write(m_outBuffer);

    if (sent == m_outBuffer.size())
        m_outBuffer.clear();
    else
        m_outBuffer.remove(0, sent);
}


void IBClient::onConnected()
{
    qDebug() << "TWS is connected";
}

void IBClient::onReadyRead()
{
    m_inBuffer.append(m_socket->readAll());

    // I'M GETTING EMPTY MESSAGES.. IS THIS THE CORRECT WAY TO HANDLE THIS ???

    if (m_inBuffer.isEmpty()) {
        qDebug() << "Received empty onReadyRead message.. ignoring it..";
        return;
    }

    // WHY ARE MESSAGES FROM TWS SARTING WITH A '\0' ??? ... IS THIS THE CORRECT WAY TO HANDLE IT?
    if (m_inBuffer.startsWith('\0')) {
        qDebug() << "[DEBUG] onReadyRead message started with a '\0' ??? wtf, I'm removing it";
        m_inBuffer.remove(0, 1);
    }

    // DEBUG
    qDebug() << "onReadyRead msg:" << m_inBuffer;

    if (!m_connected) {
        decodeField(m_serverVersion);

        if (m_serverVersion >= 20)
            decodeField(m_twsTime);

        if (m_serverVersion < SERVER_VERSION) {
            m_socket->disconnectFromHost();
            qCritical("The TWS is out of date and must be upgraded.");
        }

        m_connected = true;

        cleanInBuffer();

        // DEBUG
//        qDebug() << "server version:" << m_serverVersion;
//        qDebug() << "tws time:" << m_twsTime;
//        qDebug() << "left over:" << decodeField(m_inBuffer);

        // send the clientId
        if (m_serverVersion >= 3) {
            if (m_serverVersion < MIN_SERVER_VER_LINKING) {
                encodeField(m_clientId);
                send();
            }
            else if (!m_extraAuth) {
                const int VERSION = 1;
                encodeField(START_API);
                encodeField(VERSION);
                encodeField(m_clientId);
                send();
            }
        }
    }

    else { // yes we're connected

        int msgId;
        decodeField(msgId);

        switch (msgId) {
        case TICK_PRICE:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            double price;
            int size;
            int canAutoExecute;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(price);
            decodeField(size);
            decodeField(canAutoExecute);

            emit tickPrice(tickerId, (TickType)tickTypeInt, price, canAutoExecute);

            // process version 2 fields here
            {
                TickType sizeTickType = NOT_SET;
                switch ((TickType)tickTypeInt) {
                case BID:
                    sizeTickType = BID_SIZE;
                    break;
                case ASK:
                    sizeTickType = ASK_SIZE;
                    break;
                case LAST:
                    sizeTickType = LAST_SIZE;
                    break;
                }
                if (sizeTickType != NOT_SET)
                    emit tickSize(tickerId, sizeTickType, size);
            }
            break;
        }
        case TICK_SIZE:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            int size;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(size);

            emit tickSize(tickerId, (TickType)tickTypeInt, size);
            break;
        }
        case TICK_OPTION_COMPUTATION:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            double impliedVol;
            double delta;

            double optPrice = DBL_MAX;
            double pvDividend = DBL_MAX;
            double gamma = DBL_MAX;
            double vega = DBL_MAX;
            double theta = DBL_MAX;
            double undPrice = DBL_MAX;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(impliedVol);
            decodeField(delta);

            if (impliedVol < 0)
                impliedVol = DBL_MAX;
            if ((delta > 1) || (delta < -1))
                delta = DBL_MAX;
            if ((version >= 6) || tickTypeInt == MODEL_OPTION) {
                decodeField(optPrice);
                decodeField(pvDividend);

                if (optPrice < 0)
                    optPrice = DBL_MAX;
                if (pvDividend < 0)
                    pvDividend = DBL_MAX;
            }
            if (version >= 6) {
                decodeField(gamma);
                decodeField(vega);
                decodeField(theta);
                decodeField(undPrice);

                if (gamma > 1 || gamma < -1)
                    gamma = DBL_MAX;
                if (vega > 1 || vega < -1)
                    vega = DBL_MAX;
                if (theta > 1 || theta < -1)
                    theta = DBL_MAX;
                if (undPrice < 0)
                    undPrice = DBL_MAX;
            }
            emit tickOptionComputation(tickerId, (TickType)tickTypeInt, impliedVol, delta, optPrice, pvDividend, gamma, vega, theta, undPrice);
            break;
        }
        case TICK_GENERIC:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            double value;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(value);

            emit tickGeneric(tickerId, (TickType)tickTypeInt, value);
            break;
        }
        case TICK_STRING:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            QByteArray value;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(value);

            emit tickString(tickerId, (TickType)tickTypeInt, value);
            break;
        }
        case TICK_EFP:
        {
            int version;
            int tickerId;
            int tickTypeInt;
            double basisPoints;
            QByteArray formattedBasisPoints;
            double impliedFuturesPrice;
            int holdDays;
            QByteArray futureExpiry;
            double dividendImpact;
            double dividendsToExpiry;

            decodeField(version);
            decodeField(tickerId);
            decodeField(tickTypeInt);
            decodeField(basisPoints);
            decodeField(formattedBasisPoints);
            decodeField(impliedFuturesPrice);
            decodeField(holdDays);
            decodeField(futureExpiry);
            decodeField(dividendImpact);
            decodeField(dividendsToExpiry);

            emit tickEFP(tickerId, (TickType)tickTypeInt, basisPoints, formattedBasisPoints, impliedFuturesPrice, holdDays, futureExpiry, dividendImpact, dividendsToExpiry);
            break;
        }
        case ORDER_STATUS:
        {
            int version;
            int orderId;
            QByteArray status;
            int filled;
            int remaining;
            double avgFillPrice;
            int permId;
            int parentId;
            double lastFillPrice;
            int clientId;
            QByteArray whyHeld;

            decodeField(version);
            decodeField(orderId);
            decodeField(status);
            decodeField(filled);
            decodeField(remaining);
            decodeField(avgFillPrice);
            decodeField(permId);
            decodeField(parentId);
            decodeField(lastFillPrice);
            decodeField(clientId);
            decodeField(whyHeld);

            emit orderStatus(orderId, status, filled, remaining, avgFillPrice, permId, parentId, lastFillPrice, clientId, whyHeld);
            break;
        }
        case ERR_MSG:
        {
            int version;
            int id;
            int errorCode;
            QByteArray errorMsg;

            decodeField(version);
            decodeField(id);
            decodeField(errorCode);
            decodeField(errorMsg);

            emit error(id, errorCode, errorMsg);
            break;
        }
        case OPEN_ORDER:
        {
            // read version
            int version;
            decodeField(version);

            // read order id
            Order order;
            decodeField(order.orderId);

            // read contract fields
            Contract contract;
            decodeField(contract.conId); // ver 17 field
            decodeField(contract.symbol);
            decodeField(contract.secType);
            decodeField(contract.expiry);
            decodeField(contract.strike);
            decodeField(contract.right);
            if (version >= 32) {
                decodeField(contract.multiplier);
            }
            decodeField(contract.exchange);
            decodeField(contract.currency);
            decodeField(contract.localSymbol); // ver 2 field
            if (version >= 32) {
                decodeField(contract.tradingClass);
            }

            // read order fields
            decodeField(order.action);
            decodeField(order.totalQuantity);
            decodeField(order.orderType);
            if (version < 29) {
                decodeField(order.lmtPrice);
            }
            else {
                decodeFieldMax( order.lmtPrice);
            }
            if (version < 30) {
                decodeField(order.auxPrice);
            }
            else {
                decodeFieldMax( order.auxPrice);
            }
            decodeField(order.tif);
            decodeField(order.ocaGroup);
            decodeField(order.account);
            decodeField(order.openClose);

            int orderOriginInt;
            decodeField(orderOriginInt);
            order.origin = (Origin)orderOriginInt;

            decodeField(order.orderRef);
            decodeField(order.clientId); // ver 3 field
            decodeField(order.permId); // ver 4 field

            //if( version < 18) {
            //	// will never happen
            //	/* order.ignoreRth = */ readBoolFromInt();
            //}

            decodeField(order.outsideRth); // ver 18 field
            decodeField(order.hidden); // ver 4 field
            decodeField(order.discretionaryAmt); // ver 4 field
            decodeField(order.goodAfterTime); // ver 5 field

            {
                QByteArray sharesAllocation;
                decodeField(sharesAllocation); // deprecated ver 6 field
            }

            decodeField(order.faGroup); // ver 7 field
            decodeField(order.faMethod); // ver 7 field
            decodeField(order.faPercentage); // ver 7 field
            decodeField(order.faProfile); // ver 7 field

            decodeField(order.goodTillDate); // ver 8 field

            decodeField(order.rule80A); // ver 9 field
            decodeFieldMax( order.percentOffset); // ver 9 field
            decodeField(order.settlingFirm); // ver 9 field
            decodeField(order.shortSaleSlot); // ver 9 field
            decodeField(order.designatedLocation); // ver 9 field
            if( m_serverVersion == MIN_SERVER_VER_SSHORTX_OLD){
                int exemptCode;
                decodeField(exemptCode);
            }
            else if( version >= 23){
                decodeField(order.exemptCode);
            }
            decodeField(order.auctionStrategy); // ver 9 field
            decodeFieldMax( order.startingPrice); // ver 9 field
            decodeFieldMax( order.stockRefPrice); // ver 9 field
            decodeFieldMax( order.delta); // ver 9 field
            decodeFieldMax( order.stockRangeLower); // ver 9 field
            decodeFieldMax( order.stockRangeUpper); // ver 9 field
            decodeField(order.displaySize); // ver 9 field

            //if( version < 18) {
            //		// will never happen
            //		/* order.rthOnly = */ readBoolFromInt();
            //}

            decodeField(order.blockOrder); // ver 9 field
            decodeField(order.sweepToFill); // ver 9 field
            decodeField(order.allOrNone); // ver 9 field
            decodeFieldMax( order.minQty); // ver 9 field
            decodeField(order.ocaType); // ver 9 field
            decodeField(order.eTradeOnly); // ver 9 field
            decodeField(order.firmQuoteOnly); // ver 9 field
            decodeFieldMax( order.nbboPriceCap); // ver 9 field

            decodeField(order.parentId); // ver 10 field
            decodeField(order.triggerMethod); // ver 10 field

            decodeFieldMax( order.volatility); // ver 11 field
            decodeField(order.volatilityType); // ver 11 field
            decodeField(order.deltaNeutralOrderType); // ver 11 field (had a hack for ver 11)
            decodeFieldMax( order.deltaNeutralAuxPrice); // ver 12 field

            if (version >= 27 && !order.deltaNeutralOrderType.isEmpty()) {
                decodeField(order.deltaNeutralConId);
                decodeField(order.deltaNeutralSettlingFirm);
                decodeField(order.deltaNeutralClearingAccount);
                decodeField(order.deltaNeutralClearingIntent);
            }

            if (version >= 31 && !order.deltaNeutralOrderType.isEmpty()) {
                decodeField(order.deltaNeutralOpenClose);
                decodeField(order.deltaNeutralShortSale);
                decodeField(order.deltaNeutralShortSaleSlot);
                decodeField(order.deltaNeutralDesignatedLocation);
            }

            decodeField(order.continuousUpdate); // ver 11 field

            // will never happen
            //if( m_serverVersion == 26) {
            //	order.stockRangeLower = readDouble();
            //	order.stockRangeUpper = readDouble();
            //}

            decodeField(order.referencePriceType); // ver 11 field

            decodeFieldMax( order.trailStopPrice); // ver 13 field

            if (version >= 30) {
                decodeFieldMax( order.trailingPercent);
            }

            decodeFieldMax( order.basisPoints); // ver 14 field
            decodeFieldMax( order.basisPointsType); // ver 14 field
            decodeField(contract.comboLegsDescrip); // ver 14 field

            if (version >= 29) {
            int comboLegsCount = 0;
                decodeField(comboLegsCount);

                if (comboLegsCount > 0) {
                    QList<ComboLeg*> comboLegs;
                    for (int i = 0; i < comboLegsCount; ++i) {
                        ComboLeg* comboLeg = new ComboLeg();
                        decodeField(comboLeg->conId);
                        decodeField(comboLeg->ratio);
                        decodeField(comboLeg->action);
                        decodeField(comboLeg->exchange);
                        decodeField(comboLeg->openClose);
                        decodeField(comboLeg->shortSaleSlot);
                        decodeField(comboLeg->designatedLocation);
                        decodeField(comboLeg->exemptCode);

                        comboLegs.append(comboLeg);
                }
                    contract.comboLegs = comboLegs;
                }

                int orderComboLegsCount = 0;
                decodeField(orderComboLegsCount);
                if (orderComboLegsCount > 0) {
                    QList<OrderComboLeg*> orderComboLegs;
                    for (int i = 0; i < orderComboLegsCount; ++i) {
                        OrderComboLeg* orderComboLeg = new OrderComboLeg();
                        decodeFieldMax( orderComboLeg->price);

                        orderComboLegs.append(orderComboLeg);
                    }
                    order.orderComboLegs = orderComboLegs;
                }
            }

            if (version >= 26) {
                int smartComboRoutingParamsCount = 0;
                decodeField(smartComboRoutingParamsCount);
                if( smartComboRoutingParamsCount > 0) {
                    QList<TagValue*> smartComboRoutingParams;
                    for( int i = 0; i < smartComboRoutingParamsCount; ++i) {
                        TagValue* tagValue = new TagValue();
                        decodeField(tagValue->tag);
                        decodeField(tagValue->value);
                        smartComboRoutingParams.append(tagValue);
                    }
                    order.smartComboRoutingParams = smartComboRoutingParams;
                }
            }

            if( version >= 20) {
                decodeFieldMax( order.scaleInitLevelSize);
                decodeFieldMax( order.scaleSubsLevelSize);
            }
            else {
                // ver 15 fields
                int notSuppScaleNumComponents = 0;
                decodeFieldMax( notSuppScaleNumComponents);
                decodeFieldMax( order.scaleInitLevelSize); // scaleComponectSize
            }
            decodeFieldMax( order.scalePriceIncrement); // ver 15 field

            if (version >= 28 && order.scalePriceIncrement > 0.0 && order.scalePriceIncrement != UNSET_DOUBLE) {
                decodeFieldMax( order.scalePriceAdjustValue);
                decodeFieldMax( order.scalePriceAdjustInterval);
                decodeFieldMax( order.scaleProfitOffset);
                decodeField(order.scaleAutoReset);
                decodeFieldMax( order.scaleInitPosition);
                decodeFieldMax( order.scaleInitFillQty);
                decodeField(order.scaleRandomPercent);
            }

            if( version >= 24) {
                decodeField(order.hedgeType);
                if( !order.hedgeType.isEmpty()) {
                    decodeField(order.hedgeParam);
                }
            }

            if( version >= 25) {
                decodeField(order.optOutSmartRouting);
            }

            decodeField(order.clearingAccount); // ver 19 field
            decodeField(order.clearingIntent); // ver 19 field

            if( version >= 22) {
                decodeField(order.notHeld);
            }

            UnderComp underComp;
            if( version >= 20) {
                bool underCompPresent = false;
                decodeField(underCompPresent);
                if( underCompPresent){
                    decodeField(underComp.conId);
                    decodeField(underComp.delta);
                    decodeField(underComp.price);
                    contract.underComp = &underComp;
                }
            }


            if( version >= 21) {
                decodeField(order.algoStrategy);
                if( !order.algoStrategy.isEmpty()) {
                    int algoParamsCount = 0;
                    decodeField(algoParamsCount);
                    if( algoParamsCount > 0) {
                        for( int i = 0; i < algoParamsCount; ++i) {
                            TagValue* tagValue = new TagValue();
                            decodeField(tagValue->tag);
                            decodeField(tagValue->value);
                            order.algoParams.append( tagValue);
                        }
                    }
                }
            }

            OrderState orderState;

            decodeField(order.whatIf); // ver 16 field

            decodeField(orderState.status); // ver 16 field
            decodeField(orderState.initMargin); // ver 16 field
            decodeField(orderState.maintMargin); // ver 16 field
            decodeField(orderState.equityWithLoan); // ver 16 field
            decodeFieldMax( orderState.commission); // ver 16 field
            decodeFieldMax( orderState.minCommission); // ver 16 field
            decodeFieldMax( orderState.maxCommission); // ver 16 field
            decodeField(orderState.commissionCurrency); // ver 16 field
            decodeField(orderState.warningText); // ver 16 field

            emit openOrder( (OrderId)order.orderId, contract, order, orderState);
            break;
        }
        case ACCT_VALUE:
        {
            int version;
            QByteArray key;
            QByteArray val;
            QByteArray cur;
            QByteArray accountName;

            decodeField(version);
            decodeField(key);
            decodeField(val);
            decodeField(cur);
            decodeField(accountName); // ver 2 field

            emit updateAccountValue( key, val, cur, accountName);
            break;
        }

        case PORTFOLIO_VALUE:
        {
            // decode version
            int version;
            decodeField(version);

            // read contract fields
            Contract contract;
            decodeField(contract.conId); // ver 6 field
            decodeField(contract.symbol);
            decodeField(contract.secType);
            decodeField(contract.expiry);
            decodeField(contract.strike);
            decodeField(contract.right);

            if( version >= 7) {
                decodeField(contract.multiplier);
                decodeField(contract.primaryExchange);
            }

            decodeField(contract.currency);
            decodeField(contract.localSymbol); // ver 2 field
            if (version >= 8) {
                decodeField(contract.tradingClass);
            }

            int     position;
            double  marketPrice;
            double  marketValue;
            double  averageCost;
            double  unrealizedPNL;
            double  realizedPNL;

            decodeField(position);
            decodeField(marketPrice);
            decodeField(marketValue);
            decodeField(averageCost); // ver 3 field
            decodeField(unrealizedPNL); // ver 3 field
            decodeField(realizedPNL); // ver 3 field

            QByteArray accountName;
            decodeField(accountName); // ver 4 field
            if( version == 6 && m_serverVersion == 39) {
                decodeField(contract.primaryExchange);
            }

            emit updatePortfolio( contract,
                position, marketPrice, marketValue, averageCost,
                unrealizedPNL, realizedPNL, accountName);

            break;
        }
        case ACCT_UPDATE_TIME:
        {
            int version;
            QByteArray accountTime;

            decodeField(version);
            decodeField(accountTime);

            emit updateAccountTime( accountTime);
            break;
        }

        case NEXT_VALID_ID:
        {
            int version;
            int orderId;

            decodeField(version);
            decodeField(orderId);

            emit nextValidId(orderId);
            break;
        }

        case CONTRACT_DATA:
        {
            int version;
            decodeField(version);

            int reqId = -1;
            if( version >= 3) {
                decodeField(reqId);
            }

            ContractDetails contract;
            decodeField(contract.summary.symbol);
            decodeField(contract.summary.secType);
            decodeField(contract.summary.expiry);
            decodeField(contract.summary.strike);
            decodeField(contract.summary.right);
            decodeField(contract.summary.exchange);
            decodeField(contract.summary.currency);
            decodeField(contract.summary.localSymbol);
            decodeField(contract.marketName);
            decodeField(contract.summary.tradingClass);
            decodeField(contract.summary.conId);
            decodeField(contract.minTick);
            decodeField(contract.summary.multiplier);
            decodeField(contract.orderTypes);
            decodeField(contract.validExchanges);
            decodeField(contract.priceMagnifier); // ver 2 field
            if( version >= 4) {
                decodeField(contract.underConId);
            }
            if( version >= 5) {
                decodeField(contract.longName);
                decodeField(contract.summary.primaryExchange);
            }
            if( version >= 6) {
                decodeField(contract.contractMonth);
                decodeField(contract.industry);
                decodeField(contract.category);
                decodeField(contract.subcategory);
                decodeField(contract.timeZoneId);
                decodeField(contract.tradingHours);
                decodeField(contract.liquidHours);
            }
            if( version >= 8) {
                decodeField(contract.evRule);
                decodeField(contract.evMultiplier);
            }
            if( version >= 7) {
                int secIdListCount = 0;
                decodeField(secIdListCount);
                if( secIdListCount > 0) {
                    for( int i = 0; i < secIdListCount; ++i) {
                        TagValue* tagValue = new TagValue();
                        decodeField(tagValue->tag);
                        decodeField(tagValue->value);
                        contract.secIdList.append( tagValue);
                    }
                }
            }

            emit contractDetails( reqId, contract);
            break;
        }

        case BOND_CONTRACT_DATA:
        {
            int version;
            decodeField(version);

            int reqId = -1;
            if( version >= 3) {
                decodeField(reqId);
            }

            ContractDetails contract;
            decodeField(contract.summary.symbol);
            decodeField(contract.summary.secType);
            decodeField(contract.cusip);
            decodeField(contract.coupon);
            decodeField(contract.maturity);
            decodeField(contract.issueDate);
            decodeField(contract.ratings);
            decodeField(contract.bondType);
            decodeField(contract.couponType);
            decodeField(contract.convertible);
            decodeField(contract.callable);
            decodeField(contract.putable);
            decodeField(contract.descAppend);
            decodeField(contract.summary.exchange);
            decodeField(contract.summary.currency);
            decodeField(contract.marketName);
            decodeField(contract.summary.tradingClass);
            decodeField(contract.summary.conId);
            decodeField(contract.minTick);
            decodeField(contract.orderTypes);
            decodeField(contract.validExchanges);
            decodeField(contract.nextOptionDate); // ver 2 field
            decodeField(contract.nextOptionType); // ver 2 field
            decodeField(contract.nextOptionPartial); // ver 2 field
            decodeField(contract.notes); // ver 2 field
            if( version >= 4) {
                decodeField(contract.longName);
            }
            if( version >= 6) {
                decodeField(contract.evRule);
                decodeField(contract.evMultiplier);
            }
            if( version >= 5) {
                int secIdListCount = 0;
                decodeField(secIdListCount);
                if( secIdListCount > 0) {
                    for( int i = 0; i < secIdListCount; ++i) {
                        TagValue* tagValue = new TagValue();
                        decodeField(tagValue->tag);
                        decodeField(tagValue->value);
                        contract.secIdList.append(tagValue);
                    }
                }
            }

            emit bondContractDetails( reqId, contract);
            break;
        }

        case EXECUTION_DATA:
        {
            int version;
            decodeField(version);

            int reqId = -1;
            if( version >= 7) {
                decodeField(reqId);
            }

            int orderId;
            decodeField(orderId);

            // decode contract fields
            Contract contract;
            decodeField(contract.conId); // ver 5 field
            decodeField(contract.symbol);
            decodeField(contract.secType);
            decodeField(contract.expiry);
            decodeField(contract.strike);
            decodeField(contract.right);
            if( version >= 9) {
                decodeField(contract.multiplier);
            }
            decodeField(contract.exchange);
            decodeField(contract.currency);
            decodeField(contract.localSymbol);
            if (version >= 10) {
                decodeField(contract.tradingClass);
            }

            // decode execution fields
            Execution exec;
            exec.orderId = orderId;
            decodeField(exec.execId);
            decodeField(exec.time);
            decodeField(exec.acctNumber);
            decodeField(exec.exchange);
            decodeField(exec.side);
            decodeField(exec.shares);
            decodeField(exec.price);
            decodeField(exec.permId); // ver 2 field
            decodeField(exec.clientId); // ver 3 field
            decodeField(exec.liquidation); // ver 4 field

            if( version >= 6) {
                decodeField(exec.cumQty);
                decodeField(exec.avgPrice);
            }

            if( version >= 8) {
                decodeField(exec.orderRef);
            }

            if( version >= 9) {
                decodeField(exec.evRule);
                decodeField(exec.evMultiplier);
            }

            emit execDetails( reqId, contract, exec);
            break;
        }

        case MARKET_DEPTH:
        {
            int version;
            int id;
            int position;
            int operation;
            int side;
            double price;
            int size;

            decodeField(version);
            decodeField(id);
            decodeField(position);
            decodeField(operation);
            decodeField(side);
            decodeField(price);
            decodeField(size);

            emit updateMktDepth( id, position, operation, side, price, size);
            break;
        }

        case MARKET_DEPTH_L2:
        {
            int version;
            int id;
            int position;
            QByteArray marketMaker;
            int operation;
            int side;
            double price;
            int size;

            decodeField(version);
            decodeField(id);
            decodeField(position);
            decodeField(marketMaker);
            decodeField(operation);
            decodeField(side);
            decodeField(price);
            decodeField(size);

            emit updateMktDepthL2( id, position, marketMaker, operation, side,
                price, size);

            break;
        }

        case NEWS_BULLETINS:
        {
            int version;
            int msgId;
            int msgType;
            QByteArray newsMessage;
            QByteArray originatingExch;

            decodeField(version);
            decodeField(msgId);
            decodeField(msgType);
            decodeField(newsMessage);
            decodeField(originatingExch);

            emit updateNewsBulletin( msgId, msgType, newsMessage, originatingExch);
            break;
        }

        case MANAGED_ACCTS:
        {
            int version;
            QByteArray accountsList;

            decodeField(version);
            decodeField(accountsList);

            emit managedAccounts( accountsList);
            break;
        }

        case RECEIVE_FA:
        {
            int version;
            int faDataTypeInt;
            QByteArray cxml;

            decodeField(version);
            decodeField(faDataTypeInt);
            decodeField(cxml);

            emit receiveFA( (FaDataType)faDataTypeInt, cxml);
            break;
        }

        case HISTORICAL_DATA:
        {
            int version;
            int reqId;
            QByteArray startDateStr;
            QByteArray endDateStr;

            decodeField(version);
            decodeField(reqId);
            decodeField(startDateStr); // ver 2 field
            decodeField(endDateStr); // ver 2 field

            int itemCount;
            decodeField(itemCount);

            QVector<BarData> bars;


            for( int ctr = 0; ctr < itemCount; ++ctr) {

                BarData bar;
                decodeField(bar.date);
                decodeField(bar.open);
                decodeField(bar.high);
                decodeField(bar.low);
                decodeField(bar.close);
                decodeField(bar.volume);
                decodeField(bar.average);
                decodeField(bar.hasGaps);
                decodeField(bar.barCount); // ver 3 field

                bars.push_back(bar);
            }

//            assert( (int)bars.size() == itemCount);

            for( int ctr = 0; ctr < itemCount; ++ctr) {

                const BarData& bar = bars[ctr];
                emit historicalData( reqId, bar.date, bar.open, bar.high, bar.low,
                    bar.close, bar.volume, bar.barCount, bar.average,
                    (bar.hasGaps == "true" ? 1 : 0));
            }

            // send end of dataset marker
            QByteArray finishedStr = QByteArray("finished-") + startDateStr + "-" + endDateStr;
            emit historicalData( reqId, finishedStr, -1, -1, -1, -1, -1, -1, -1, 0);
            break;
        }

        case SCANNER_DATA:
        {
            int version;
            int tickerId;

            decodeField(version);
            decodeField(tickerId);

            int numberOfElements;
            decodeField(numberOfElements);

            typedef std::vector<ScanData> ScanDataList;
            ScanDataList scannerDataList;

            scannerDataList.reserve( numberOfElements);

            for( int ctr=0; ctr < numberOfElements; ++ctr) {

                ScanData data;

                decodeField(data.rank);
                decodeField(data.contract.summary.conId); // ver 3 field
                decodeField(data.contract.summary.symbol);
                decodeField(data.contract.summary.secType);
                decodeField(data.contract.summary.expiry);
                decodeField(data.contract.summary.strike);
                decodeField(data.contract.summary.right);
                decodeField(data.contract.summary.exchange);
                decodeField(data.contract.summary.currency);
                decodeField(data.contract.summary.localSymbol);
                decodeField(data.contract.marketName);
                decodeField(data.contract.summary.tradingClass);
                decodeField(data.distance);
                decodeField(data.benchmark);
                decodeField(data.projection);
                decodeField(data.legsStr);

                scannerDataList.push_back( data);
            }

//            assert( (int)scannerDataList.size() == numberOfElements);

            for( int ctr=0; ctr < numberOfElements; ++ctr) {

                const ScanData& data = scannerDataList[ctr];
                emit scannerData( tickerId, data.rank, data.contract,
                    data.distance, data.benchmark, data.projection, data.legsStr);
            }

            emit scannerDataEnd( tickerId);
            break;
        }

        case SCANNER_PARAMETERS:
        {
            int version;
            QByteArray xml;

            decodeField(version);
            decodeField(xml);

            emit scannerParameters( xml);
            break;
        }

        case CURRENT_TIME:
        {
            int version;
            int time;

            decodeField(version);
            decodeField(time);

            emit currentTime( time);
            break;
        }

        case REAL_TIME_BARS:
        {
            int version;
            int reqId;
            int time;
            double open;
            double high;
            double low;
            double close;
            int volume;
            double average;
            int count;

            decodeField(version);
            decodeField(reqId);
            decodeField(time);
            decodeField(open);
            decodeField(high);
            decodeField(low);
            decodeField(close);
            decodeField(volume);
            decodeField(average);
            decodeField(count);

            emit realtimeBar( reqId, time, open, high, low, close,
                volume, average, count);

            break;
        }

        case FUNDAMENTAL_DATA:
        {
            int version;
            int reqId;
            QByteArray data;

            decodeField(version);
            decodeField(reqId);
            decodeField(data);

            emit fundamentalData( reqId, data);
            break;
        }

        case CONTRACT_DATA_END:
        {
            int version;
            int reqId;

            decodeField(version);
            decodeField(reqId);

            emit contractDetailsEnd( reqId);
            break;
        }

        case OPEN_ORDER_END:
        {
            int version;

            decodeField(version);

            emit openOrderEnd();
            break;
        }

        case ACCT_DOWNLOAD_END:
        {
            int version;
            QByteArray account;

            decodeField(version);
            decodeField(account);

            emit accountDownloadEnd( account);
            break;
        }

        case EXECUTION_DATA_END:
        {
            int version;
            int reqId;

            decodeField(version);
            decodeField(reqId);

            emit execDetailsEnd( reqId);
            break;
        }

        case DELTA_NEUTRAL_VALIDATION:
        {
            int version;
            int reqId;

            decodeField(version);
            decodeField(reqId);

            UnderComp underComp;

            decodeField(underComp.conId);
            decodeField(underComp.delta);
            decodeField(underComp.price);

            emit deltaNeutralValidation( reqId, underComp);
            break;
        }

        case TICK_SNAPSHOT_END:
        {
            int version;
            int reqId;

            decodeField(version);
            decodeField(reqId);

            emit tickSnapshotEnd( reqId);
            break;
        }

        case MARKET_DATA_TYPE:
        {
            int version;
            int reqId;
            int marketDataTypeVal;

            decodeField(version);
            decodeField(reqId);
            decodeField(marketDataTypeVal);

            emit marketDataType( reqId, marketDataTypeVal);
            break;
        }

        case COMMISSION_REPORT:
        {
            int version;
            decodeField(version);

            CommissionReport cr;
            decodeField(cr.execId);
            decodeField(cr.commission);
            decodeField(cr.currency);
            decodeField(cr.realizedPNL);
            decodeField(cr.yield);
            decodeField(cr.yieldRedemptionDate);

            emit commissionReport( cr);
            break;
        }

        case POSITION_DATA:
        {
            int version;
            QByteArray account;
            int pos;
            double avgCost = 0;

            decodeField(version);
            decodeField(account);

            // decode contract fields
            Contract contract;
            decodeField(contract.conId);
            decodeField(contract.symbol);
            decodeField(contract.secType);
            decodeField(contract.expiry);
            decodeField(contract.strike);
            decodeField(contract.right);
            decodeField(contract.multiplier);
            decodeField(contract.exchange);
            decodeField(contract.currency);
            decodeField(contract.localSymbol);
            if (version >= 2) {
                decodeField(contract.tradingClass);
            }

            decodeField(pos);
            if (version >= 3) {
                decodeField(avgCost);
            }

            emit position( account, contract, pos, avgCost);
            break;
        }

        case POSITION_END:
        {
            int version;

            decodeField(version);

            emit positionEnd();
            break;
        }

        case ACCOUNT_SUMMARY:
        {
            int version;
            int reqId;
            QByteArray account;
            QByteArray tag;
            QByteArray value;
            QByteArray curency;

            decodeField(version);
            decodeField(reqId);
            decodeField(account);
            decodeField(tag);
            decodeField(value);
            decodeField(curency);

            emit accountSummary( reqId, account, tag, value, curency);
            break;
        }

        case ACCOUNT_SUMMARY_END:
        {
            int version;
            int reqId;

            decodeField(version);
            decodeField(reqId);

            emit accountSummaryEnd( reqId);
            break;
        }

        case VERIFY_MESSAGE_API:
        {
            int version;
            QByteArray apiData;

            decodeField(version);
            decodeField(apiData);

            emit verifyMessageAPI( apiData);
            break;
        }

        case VERIFY_COMPLETED:
        {
            int version;
            QByteArray isSuccessful;
            QByteArray errorText;

            decodeField(version);
            decodeField(isSuccessful);
            decodeField(errorText);

            bool bRes = isSuccessful == "true";

            if (bRes) {
                const int VERSION = 1;
                encodeField(START_API);
                encodeField(VERSION);
                encodeField(m_clientId);
                send();
            }

            emit verifyCompleted( bRes, errorText);
            break;
        }

        case DISPLAY_GROUP_LIST:
        {
            int version;
            int reqId;
            QByteArray groups;

            decodeField(version);
            decodeField(reqId);
            decodeField(groups);

            emit displayGroupList( reqId, groups);
            break;
        }

        case DISPLAY_GROUP_UPDATED:
        {
            int version;
            int reqId;
            QByteArray contractInfo;

            decodeField(version);
            decodeField(reqId);
            decodeField(contractInfo);

            emit displayGroupUpdated( reqId, contractInfo);
            break;
        }

        default:
        {
            emit error( msgId, UNKNOWN_ID.code(), UNKNOWN_ID.msg());
            disconnectTWS();
            emit connectionClosed();
            break;
        }
        }
    }
}



void IBClient::decodeField(int &value)
{
    value = decodeField().toInt();
}

void IBClient::decodeField(bool &value)
{
    value = (decodeField().toInt() ? 1 : 0);
}

void IBClient::decodeField(long &value)
{
    value = decodeField().toLong();
}

void IBClient::decodeField(double &value)
{
    value = decodeField().toDouble();
}

void IBClient::decodeField(QByteArray & value)
{
    value = decodeField();
}

QByteArray IBClient::decodeField()
{
    QByteArray ret;
    m_endIdx = m_inBuffer.indexOf('\0', m_begIdx);
    ret = m_inBuffer.mid(m_begIdx, m_endIdx - m_begIdx);
    m_begIdx = m_endIdx + 1;
    return ret;
}

void IBClient::decodeFieldMax(int &value)
{
    QByteArray str;
    decodeField(str);
    value = (str.isEmpty() ? UNSET_INTEGER : str.toInt());
}

void IBClient::decodeFieldMax(long &value)
{
    QByteArray str;
    decodeField(str);
    value = (str.isEmpty() ? UNSET_INTEGER : str.toLong());
}

void IBClient::decodeFieldMax(double &value)
{
    QByteArray str;
    decodeField(str);
    value = (str.isEmpty() ? UNSET_DOUBLE : str.toDouble());
}

void IBClient::encodeField(const int &value)
{
    encodeField(QByteArray::number(value));
}

void IBClient::encodeField(const bool &value)
{
    encodeField(QByteArray::number((value ? 1 : 0)));
}

void IBClient::encodeField(const long &value)
{
    encodeField(QByteArray::number((int)value));
}

void IBClient::encodeField(const double &value)
{
    encodeField(QByteArray::number(value));
}

void IBClient::encodeField(const QByteArray &buf)
{
    m_outBuffer.append(buf);
    m_outBuffer.append('\0');
}

void IBClient::cleanInBuffer()
{
    if (m_endIdx == m_inBuffer.size()) {
        m_inBuffer.clear();
        m_begIdx = m_endIdx = 0;
    }
    else {
        m_inBuffer.remove(0, m_endIdx);
        m_begIdx = 0;
    }
}

