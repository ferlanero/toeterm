/*
    ThumbTerm Copyright Olli Vanhoja
    FingerTerm Copyright 2011-2012 Heikki Holstila <heikki.holstila@gmail.com>
    ToeTerm Copyright 2018 ROZZ, 2019 Matti Viljanen

    This file is part of ToeTerm.

    ToeTerm is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    ToeTerm is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ToeTerm.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QGuiApplication>
#include <QClipboard>
#include <QDebug>

#include "terminal.h"
#include "ptyiface.h"
#include "textrender.h"
#include "util.h"

Terminal::Terminal(QObject *parent) :
    QObject(parent), iRenderer(0), iPtyIFace(0), iUtil(0),
    iTermSize(0,0), iEmitCursorChangeSignal(true),
    iShowCursor(true), iUseAltScreenBuffer(false), iAppCursorKeys(false)
{
    zeroChar.c = ' ';
    zeroChar.bgColor = defaultBgColor;
    zeroChar.fgColor = defaultFgColor;
    zeroChar.attrib = 0;

    escape = -1;

    iTermAttribs.currentFgColor = defaultFgColor;
    iTermAttribs.currentBgColor = defaultBgColor;
    iTermAttribs.currentAttrib = 0;
    iTermAttribs.cursorPos = QPoint(0,0);
    iMarginBottom = 0;
    iMarginTop = 0;

    resetBackBufferScrollPos();

    iTermAttribs_saved = iTermAttribs;
    iTermAttribs_saved_alt = iTermAttribs;

    resetTerminal();
}

void Terminal::setRenderer(TextRender* tr)
{
    iRenderer = tr;

    if(tr) {
        tr->updateTermSize();
        connect(this, SIGNAL(displayBufferChanged()), tr, SLOT(redraw()));
        connect(this, SIGNAL(cursorPosChanged(QPoint)), tr, SLOT(redraw()));
        connect(this, SIGNAL(termSizeChanged(QSize)), tr, SLOT(redraw()));
    } else {
        qDebug() << "warning: null text renderer";
    }
}

void Terminal::setPtyIFace(PtyIFace *pty)
{
    iPtyIFace = pty;
    if(!pty) {
        qDebug() << "warning: null pty iface";
    }
}

void Terminal::setCursorPos(QPoint pos)
{
    if( iTermAttribs.cursorPos != pos ) {
        int tlimit = 1;
        int blimit = iTermSize.height();
        if(iTermAttribs.originMode) {
            tlimit = iMarginTop;
            blimit = iMarginBottom;
        }

        if(pos.x() < 1)
            pos.setX(1);
        if(pos.x() > iTermSize.width()+1)
            pos.setX(iTermSize.width()+1);
        if(pos.y() < tlimit)
            pos.setY(tlimit);
        if(pos.y() > blimit)
            pos.setY(blimit);

        iTermAttribs.cursorPos=pos;
        if(iEmitCursorChangeSignal)
            emit cursorPosChanged(pos);
    }
}

QPoint Terminal::cursorPos()
{
    return iTermAttribs.cursorPos;
}

bool Terminal::showCursor()
{
    if(iBackBufferScrollPos != 0)
        return false;

    return iShowCursor;
}

QList<QList<TermChar> >& Terminal::buffer()
{
    if(iUseAltScreenBuffer)
        return iAltBuffer;

    return iBuffer;
}

void Terminal::setTermSize(QSize size)
{
    if( iTermSize != size ) {
        iMarginTop = 1;
        iMarginBottom = size.height();
        iTermSize=size;

        resetTabs();

        emit termSizeChanged(size);
    }
}

void Terminal::putString(QString str, bool unEscape)
{
    if (unEscape) {
        str.replace("\\r", "\r");
        str.replace("\\n", "\n");
        str.replace("\\e", QChar(ch_ESC));
        str.replace("\\b", "\b");
        str.replace("\\t", "\t");

        //hex
        while(str.indexOf("\\x") != -1) {
            int i = str.indexOf("\\x")+2;
            QString num;
            while(num.length() < 2 && str.length()>i && Util::charIsHexDigit(str.at(i))) {
                num.append(str.at(i));
                i++;
            }
            str.remove(i-2-num.length(), num.length()+2);
            bool ok;
            str.insert(i-2-num.length(), QChar(num.toInt(&ok,16)));
        }
        //octal
        while(str.indexOf("\\0") != -1) {
            int i = str.indexOf("\\0")+2;
            QString num;
            while(num.length() < 3 && str.length()>i &&
                  (str.at(i).toLatin1() >= 48 && str.at(i).toLatin1() <= 55)) { //accept only 0-7
                num.append(str.at(i));
                i++;
            }
            str.remove(i-2-num.length(), num.length()+2);
            bool ok;
            str.insert(i-2-num.length(), QChar(num.toInt(&ok,8)));
        }
    }

    if(iPtyIFace)
        iPtyIFace->writeTerm(str);
}

void Terminal::keyPress(int key, int modifiers)
{
    QChar c(key);

    resetBackBufferScrollPos();

    if (c.isLetter()) {
        c = ((modifiers & Qt::ShiftModifier) != 0) ? c.toUpper() : c.toLower();
    }

    QString toWrite;

    if(key <= 0xFF || (key >= 0x410 && key <= 0x44F)) {
        if(modifiers & Qt::AltModifier)
            toWrite.append(ch_ESC);

        if (modifiers & Qt::ControlModifier) {
            char asciiVal = c.toLatin1();

            if (asciiVal >= 0x41 && asciiVal <= 0x5f) {
                // Turn uppercase characters into their control code equivalent
                toWrite.append(asciiVal - 0x40);
            } else if (asciiVal >= 0x61) {
                // Turn lowercase characters into their control code equivalent
                toWrite.append(asciiVal - 0x60);
            } else {
                qWarning() << "Ctrl+" << c << " does not translate into a control code";
            }
        } else {
            toWrite.append(c);
        }

        if(iPtyIFace)
            iPtyIFace->writeTerm(toWrite);
        return;
    }

    char cursorModif='[';
    if(iAppCursorKeys)
        cursorModif = 'O';

    if( key==Qt::Key_Up )
        toWrite += QString("%1%2A").arg(ch_ESC).arg(cursorModif).toLatin1();
    if( key==Qt::Key_Down )
        toWrite += QString("%1%2B").arg(ch_ESC).arg(cursorModif).toLatin1();
    if( key==Qt::Key_Right )
        toWrite += QString("%1%2C").arg(ch_ESC).arg(cursorModif).toLatin1();
    if( key==Qt::Key_Left )
        toWrite += QString("%1%2D").arg(ch_ESC).arg(cursorModif).toLatin1();

    if( key==Qt::Key_Enter || key==Qt::Key_Return ) {
        if(iNewLineMode)
            toWrite += "\r\n";
        else
            toWrite += "\r";
    }
    if( key==Qt::Key_Backspace )
        toWrite += "\x7F";
    if( key==Qt::Key_Tab )
        toWrite = "\t";

    if( key==Qt::Key_PageUp )
        toWrite += QString("%1[5~").arg(ch_ESC).toLatin1();
    if( key==Qt::Key_PageDown )
        toWrite += QString("%1[6~").arg(ch_ESC).toLatin1();
    if( key==Qt::Key_Home )
        toWrite += QString("%1OH").arg(ch_ESC).toLatin1();
    if( key==Qt::Key_End )
        toWrite += QString("%1OF").arg(ch_ESC).toLatin1();
    if( key==Qt::Key_Delete )
        toWrite += QString("%1[3~").arg(ch_ESC).toLatin1();
    if( key==Qt::Key_Escape )
        toWrite += QString(1,ch_ESC);
    if (key >= 0x01000030 && key <= 0x0100003b)
        toWrite += (QString("%1[").arg(ch_ESC) + QString::number(key-0x1000025) + QString("~")).toLatin1();

    if(iPtyIFace)
        iPtyIFace->writeTerm(toWrite);
}

void Terminal::insertInBuffer(const QString& chars)
{
    if(iTermSize.isNull()) {
        qDebug() << "null size terminal";
        return;
    }

    iEmitCursorChangeSignal = false;

    QChar ch;
    char latin;

    for(int i=0; i<chars.size(); i++) {
        ch = chars.at(i);
        latin = ch.toLatin1();
        if(latin=='\n' || latin==11 || latin==12) {  // line feed, vertical tab or form feed
            if(cursorPos().y()==iMarginBottom) {
                scrollFwd(1);
                if(iNewLineMode)
                    setCursorPos(QPoint(1,cursorPos().y()));
            }
            else if(cursorPos().x() <= termSize().width()) // ignore newline after <termwidth> cols (terminfo: xenl)
            {
                if(iNewLineMode)
                    setCursorPos(QPoint(1,cursorPos().y()+1));
                else
                    setCursorPos(QPoint(cursorPos().x(), cursorPos().y()+1));
            }
        }
        else if(latin=='\r') {  // carriage return
            setCursorPos(QPoint(1,cursorPos().y()));
        }
        else if(latin=='\b' || latin==127) {  //backspace & del (only move cursor, don't erase)
            setCursorPos(QPoint(cursorPos().x()-1,cursorPos().y()));
        }
        else if(latin=='\a') {  // BEL
            if(escape==']') {  // BEL also ends OSC sequence
                escape=-1;
                oscSequence(oscSeq);
                oscSeq.clear();
            } else {
                iUtil->bellAlert();
            }
        }
        else if(latin=='\t') {  //tab
            if(cursorPos().y() <= iTabStops.size()) {
                for(int i=0; i<iTabStops[cursorPos().y()-1].count(); i++) {
                    if(iTabStops[cursorPos().y()-1][i] > cursorPos().x()) {
                        setCursorPos(QPoint( iTabStops[cursorPos().y()-1][i], cursorPos().y() ));
                        break;
                    }
                }
            }
        }
        else if(latin==14 || latin==15) {  //SI and SO, related to character set... ignore
        }
        else {
            if( escape>=0 ) {
                if( escape==0 && (latin=='[') ) {
                    escape='['; //ansi sequence
                    escSeq += ch;
                }
                else if( escape==0 && (latin==']') ) {
                    escape=']'; //osc sequence
                    oscSeq += ch;
                }
                else if( escape==0 && multiCharEscapes.contains(latin)) {
                    escape = latin;
                    escSeq += ch;
                }
                else if( escape==0 && latin=='\\' ) {  // ESC\ also ends OSC sequence
                    escape=-1;
                    oscSequence(oscSeq);
                    oscSeq.clear();
                }
                else if (latin==ch_ESC) {
                    escape = 0;
                }
                else if( escape=='[' || multiCharEscapes.contains(escape) ) {
                    escSeq += ch;
                }
                else if( escape==']' ) {
                    oscSeq += ch;
                }
                else if( multiCharEscapes.contains(escape) ) {
                    escSeq += ch;
                }
                else {
                    escControlChar(QByteArray(1,latin));
                    escape=-1;
                }

                if( escape=='[' && latin >= 64 && latin <= 126 && latin != '[' ) {
                    ansiSequence(escSeq);
                    escape=-1;
                    escSeq.clear();
                }
                if( multiCharEscapes.contains(escape) && escSeq.length()>=2 ) {
                    escControlChar(escSeq);
                    escape=-1;
                    escSeq.clear();
                }
            } else {
                if (ch.isPrint())
                    insertAtCursor(ch, !iReplaceMode);
                else if (latin==ch_ESC)
                    escape=0;
                else if (latin != 0)
                    qDebug() << "unprintable char" << int(latin);
            }
        }
    }

    iEmitCursorChangeSignal = true;
    emit displayBufferChanged();
}

void Terminal::insertAtCursor(QChar c, bool overwriteMode, bool advanceCursor)
{
    if(cursorPos().x() > iTermSize.width() && advanceCursor) {
        if(iTermAttribs.wrapAroundMode) {
            if(cursorPos().y()>=iMarginBottom) {
                scrollFwd(1);
                setCursorPos(QPoint(1, cursorPos().y()));
            } else {
                setCursorPos(QPoint(1, cursorPos().y()+1));
            }
        } else {
            setCursorPos(QPoint(iTermSize.width(), cursorPos().y()));
        }
    }

    QList<TermChar> &curLine = currentLine();

    while(curLine.size() < cursorPos().x() )
        curLine.append(zeroChar);

    if(!overwriteMode)
        curLine.insert(cursorPos().x()-1,zeroChar);

    curLine[cursorPos().x()-1].c = c;
    curLine[cursorPos().x()-1].fgColor = iTermAttribs.currentFgColor;
    curLine[cursorPos().x()-1].bgColor = iTermAttribs.currentBgColor;
    curLine[cursorPos().x()-1].attrib = iTermAttribs.currentAttrib;

    if (advanceCursor) {
        setCursorPos(QPoint(cursorPos().x()+1,cursorPos().y()));
    }
}

void Terminal::deleteAt(QPoint pos)
{
    clearAt(pos);
    QList<TermChar> &curLine = buffer()[pos.y()-1];
    for (int i = pos.x(); i < curLine.length(); i++) {
        curLine[i - 1].c = curLine[i].c;
        curLine[i - 1].fgColor = curLine[i].fgColor;
        curLine[i - 1].bgColor = curLine[i].bgColor;
        curLine[i - 1].attrib = curLine[i].attrib;
    }
    curLine[curLine.length() - 1].c = ' ';
    curLine[curLine.length() - 1].fgColor = iTermAttribs.currentFgColor;
    curLine[curLine.length() - 1].bgColor = iTermAttribs.currentBgColor;
    curLine[curLine.length() - 1].attrib = iTermAttribs.currentAttrib;
}

void Terminal::clearAt(QPoint pos)
{
    if(pos.y() <= 0 || pos.y()-1 > buffer().size() ||
            pos.x() <= 0 || pos.x()-1 > buffer()[pos.y()-1].size())
    {
        qDebug() << "warning: trying to clear char out of bounds";
        return;
    }

    // just in case...
    while(buffer().size() < pos.y())
        buffer().append(QList<TermChar>());
    while(buffer()[pos.y()-1].size() < pos.x() )
        buffer()[pos.y()-1].append(zeroChar);

    buffer()[pos.y()-1][pos.x()-1].c = ' ';
    buffer()[pos.y()-1][pos.x()-1].fgColor = iTermAttribs.currentFgColor;
    buffer()[pos.y()-1][pos.x()-1].bgColor = iTermAttribs.currentBgColor;
    buffer()[pos.y()-1][pos.x()-1].attrib = iTermAttribs.currentAttrib;
}

void Terminal::eraseLineAtCursor(int from, int to)
{
    QList<TermChar> &curLine = currentLine();

    if(from==-1 && to==-1) {
        from = 1;
        to = iTermSize.width();
    }

    if(from < 1)
        from=1;
    from--;

    if (to < 1 || to > iTermSize.width())
        to = iTermSize.width();
    to--;

    if(from>to)
        return;

    while(curLine.size() <= to)
        curLine.append(zeroChar);

    for(int i=from; i<=to; i++) {
        curLine[i].c = ' ';
        curLine[i].fgColor = iTermAttribs.currentFgColor;
        curLine[i].bgColor = iTermAttribs.currentBgColor;
        curLine[i].attrib = iTermAttribs.currentAttrib;
    }
}

void Terminal::clearAll(bool wholeBuffer)
{
    clearSelection();
    if(wholeBuffer) {
        backBuffer().clear();
        resetBackBufferScrollPos();
    }
    QList<QList<TermChar>> &buf = buffer();
    for (int i = iMarginTop-1; i < iMarginBottom-1; i++) {
        while (buf.size() <= i)
            buf.append(QList<TermChar>());
        for (int j = 0; j < iTermSize.width(); j++) {
            while (buf[i].size() <= j)
                buf[i].append(zeroChar);
            buf[i][j].c = ' ';
            buf[i][j].fgColor = iTermAttribs.currentFgColor;
            buf[i][j].bgColor = iTermAttribs.currentBgColor;
            buf[i][j].attrib = iTermAttribs.currentAttrib;
        }
    }
    setCursorPos(QPoint(1,1));
}


void Terminal::ansiSequence(const QString& seq)
{
    if(seq.length() <= 1 || seq.at(0)!='[')
        return;

    QChar cmdChar = seq.at(seq.length()-1);
    QString extra;
    QList<int> params;

    int x=1;
    while(x<seq.length()-1 && !QChar(seq.at(x)).isNumber())
        x++;

    QList<QString> tmp = seq.mid(x,seq.length()-x-1).split(';');
    foreach(QString b, tmp) {
        bool ok=false;
        int t = b.toInt(&ok);
        if(ok) {
            params.append(t);
        }
    }
    if(x>1)
        extra = seq.mid(1,x-1);

    bool unhandled = false;

    switch(cmdChar.toLatin1())
    {
    case 'A': //cursor up
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), qMax(iMarginTop, cursorPos().y()-params.at(0)) ));
        break;
    case 'B': //cursor down
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), qMin(iMarginBottom, cursorPos().y()+params.at(0)) ));
        break;
    case 'C': //cursor fwd
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( qMin(iTermSize.width(),cursorPos().x()+params.at(0)), cursorPos().y() ));
        break;
    case 'D': //cursor back
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( qMax(1,cursorPos().x()-params.at(0)), cursorPos().y() ));
        break;
    case 'E': //cursor next line
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( 1, qMin(iMarginBottom, cursorPos().y()+params.at(0)) ));
        break;
    case 'F': //cursor prev line
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( 1, qMax(iMarginTop, cursorPos().y()-params.at(0)) ));
        break;
    case 'G': //go to column
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( params.at(0), cursorPos().y() ));
        break;
    case 'H': //cursor pos
    case 'f': //cursor pos
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        while(params.count()<2)
            params.append(1);
        if (iTermAttribs.originMode)
            setCursorPos(QPoint( params.at(1), params.at(0)+iMarginTop-1 ));
        else
            setCursorPos(QPoint( params.at(1), params.at(0) ));
        break;
    case 'J': //erase data
        if(!extra.isEmpty() && extra!="?") {
            unhandled=true;
            break;
        }
        if(params.count()>=1 && params.at(0)==1) {
            eraseLineAtCursor(1,cursorPos().x());
            QList<QList<TermChar>> &buf = buffer();
            for (int i = 0; i < cursorPos().y()-1; i++) {
                for (int j = 0; j < iTermSize.width(); j++) {
                    if (buf[i].size() <= j)
                        buf[i].append(zeroChar);
                    buf[i][j].c = ' ';
                    buf[i][j].fgColor = iTermAttribs.currentFgColor;
                    buf[i][j].bgColor = iTermAttribs.currentBgColor;
                    buf[i][j].attrib = iTermAttribs.currentAttrib;
                }
            }
        } else if(params.count()>=1 && params.at(0)==2) {
            clearAll();
        } else {
            eraseLineAtCursor(cursorPos().x());
            QList<QList<TermChar>> &buf = buffer();
            for (int i = cursorPos().y(); i < iTermSize.height(); i++) {
                if (buf.size() <= i)
                    buf.append(QList<TermChar>());
                for (int j = 0; j < iTermSize.width(); j++) {
                    if (buf[i].size() <= j)
                        buf[i].append(zeroChar);
                    buf[i][j].c = ' ';
                    buf[i][j].fgColor = iTermAttribs.currentFgColor;
                    buf[i][j].bgColor = iTermAttribs.currentBgColor;
                    buf[i][j].attrib = iTermAttribs.currentAttrib;
                }
            }
        }
        break;
    case 'K': //erase in line
        if(!extra.isEmpty() && extra!="?") {
            unhandled=true;
            break;
        }
        if(params.count()>=1 && params.at(0)==1) {
            eraseLineAtCursor(1,cursorPos().x());
        }
        else if(params.count()>=1 && params.at(0)==2) {
            QList<TermChar> &line = currentLine();
            for (int i = 0; i < iTermSize.width(); i++) {
                if (line.size() <= i)
                    line.append(zeroChar);
                line[i].c = ' ';
                line[i].fgColor = iTermAttribs.currentFgColor;
                line[i].bgColor = iTermAttribs.currentBgColor;
                line[i].attrib = iTermAttribs.currentAttrib;
            }
        } else {
            eraseLineAtCursor(cursorPos().x());
        }
        break;

    case 'L':  // insert lines
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(cursorPos().y() < iMarginTop || cursorPos().y() > iMarginBottom)
            break;
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        if(params.at(0) > iMarginBottom-cursorPos().y())
            scrollBack(iMarginBottom-cursorPos().y(), cursorPos().y());
        else
            scrollBack(params.at(0), cursorPos().y());
        setCursorPos(QPoint(1,cursorPos().y()));
        break;
    case 'M':  // delete lines
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(cursorPos().y() < iMarginTop || cursorPos().y() > iMarginBottom)
            break;
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        if(params.at(0) > iMarginBottom-cursorPos().y())
            scrollFwd(iMarginBottom-cursorPos().y(), cursorPos().y());
        else
            scrollFwd(params.at(0), cursorPos().y());
        setCursorPos(QPoint(1,cursorPos().y()));
        break;

    case 'P': // delete characters
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        for(int i=0; i<params.at(0); i++)
            deleteAt(cursorPos());
        break;
    case '@': // insert blank characters
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0] = 1;
        for(int i=1; i<=params.at(0); i++)
            insertAtCursor(zeroChar.c, false, false);
        break;

    case 'S':  // scroll up n lines
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        scrollFwd(params.at(0));
        break;
    case 'T':  // scroll down n lines
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        scrollBack(params.at(0));
        break;

    case 'c': // vt100 identification
        if(params.count()==0)
            params.append(0);
        if(params.count()==1 && params.at(0)==0) {
            QString toWrite = QString("%1[?1;2c").arg(ch_ESC).toLatin1();
            if(iPtyIFace)
                iPtyIFace->writeTerm(toWrite);
        } else unhandled=true;
        break;

    case 'd': //go to row
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count()<1)
            params.append(1);
        if(params.at(0)==0)
            params[0]=1;
        setCursorPos(QPoint( cursorPos().x(), params.at(0) ));
        break;

    case 'g': //tab stop manipulation
        if(params.count()==0)
            params.append(0);
        if(params.at(0)==0 && extra=="") {  //clear tab at current position
            if(cursorPos().y() <= iTabStops.size()) {
                int idx = iTabStops[cursorPos().y()-1].indexOf(cursorPos().x());
                if(idx != -1)
                    iTabStops[cursorPos().y()-1].removeAt(idx);
            }
        }
        else if(params.at(0)==3 && extra=="") {  //clear all tabs
            iTabStops.clear();
        }
        break;

    case 'n':
        if(params.count()>=1 && params.at(0)==6 && extra=="") {  // write cursor pos
            QString toWrite = QString("%1[%2;%3R").arg(ch_ESC).arg(cursorPos().y()).arg(cursorPos().x()).toLatin1();
            if(iPtyIFace)
                iPtyIFace->writeTerm(toWrite);
        } else unhandled=true;
        break;

    case 'p':
        if(extra=="!") {  // reset terminal
            resetTerminal();
        } else unhandled=true;
        break;

    case 's': //save cursor
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        iTermAttribs_saved = iTermAttribs;
        break;
    case 'u': //restore cursor
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        iTermAttribs = iTermAttribs_saved;
        break;

    case 'm': //graphics mode
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count() > 0) {
            // xterm 256-colour support
            if(params.count() > 1 && (params[0] == 38 || params[0] == 48)) {
                if(params.count() > 2 && params[1] == 5 &&
                   params[2] >= 0 && params[2] <= 255) {
                    if(params[0] == 38)
                        iTermAttribs.currentFgColor = params[2];
                    else
                        iTermAttribs.currentBgColor = params[2];
                }
                // TODO: 2;r;g;b for 24-bit colour support (Konsole etc)
                break;
            }

            if(params.contains(0)) {
                iTermAttribs.currentFgColor = defaultFgColor;
                iTermAttribs.currentBgColor = defaultBgColor;
                iTermAttribs.currentAttrib = attribNone;
            }
            if(params.contains(1))
                iTermAttribs.currentAttrib |= attribBold;
            if(params.contains(4))
                iTermAttribs.currentAttrib |= attribUnderline;
            if(params.contains(7))
                iTermAttribs.currentAttrib |= attribNegative;

            if(params.contains(22))
                iTermAttribs.currentAttrib &= ~attribBold;
            if(params.contains(24))
                iTermAttribs.currentAttrib &= ~attribUnderline;
            if(params.contains(27))
                iTermAttribs.currentAttrib &= ~attribNegative;

            foreach(int p, params) {
                if(p >= 30 && p<= 37) {
                    iTermAttribs.currentFgColor = p-30;
                }
                if(p >= 40 && p<= 47) {
                    iTermAttribs.currentBgColor = p-40;
                }
            }

            // high-intensity regular-weight extension (nonstandard)
            foreach(int p, params) {
                if(p >= 90 && p<= 97) {
                    iTermAttribs.currentFgColor = p-90+8;
                }
                if(p >= 100 && p<= 107) {
                    iTermAttribs.currentBgColor = p-100+8;
                }
            }

            if(params.contains(39))
                iTermAttribs.currentFgColor = defaultFgColor;
            if(params.contains(49))
                iTermAttribs.currentBgColor = defaultBgColor;
        } else {
            iTermAttribs.currentFgColor = defaultFgColor;
            iTermAttribs.currentBgColor = defaultBgColor;
            iTermAttribs.currentAttrib = attribNone;
        }
        break;

    case 'h':
        if(params.count()>=1 && params.contains(1) && extra=="?") { // application cursor keys
            iAppCursorKeys = true;
        }
        else if(params.count()>=1 && params.contains(3) && extra=="?") { //column mode
            // not supported, just clear screen, move cursor home & reset scrolling region
            clearAll();
            resetTabs();
            iMarginTop = 1;
            iMarginBottom = iTermSize.height();
        }
        else if(params.count()>=1 && params.contains(6) && extra=="?") { //origin mode enable
            iTermAttribs.originMode = true;
        }
        else if(params.count()>=1 && params.contains(7) && extra=="?") { //wraparound mode enable
            iTermAttribs.wrapAroundMode = true;
        }
        else if(params.count()>=1 && params.contains(12) && extra=="?") { // start blinking cursor
            // just ignore, we don't blink
        }
        else if(params.count()>=1 && params.contains(25) && extra=="?") { // show cursor
            iShowCursor = true;
        }
        else if(params.count()>=1 && params.contains(1049) && extra=="?") { //use alt screen buffer & save cursor
            iTermAttribs_saved_alt = iTermAttribs;
            iUseAltScreenBuffer = true;
            iMarginTop = 1;
            iMarginBottom = iTermSize.height();
            resetBackBufferScrollPos();

            clearAll();
            resetTabs();
            emit displayBufferChanged();
        }
        else if(params.count()>=1 && params.contains(4) && extra=="") {
            iReplaceMode = true;
        }
        else if(params.count()>=1 && params.contains(20) && extra=="") {
            iNewLineMode = true;
        }
        else unhandled=true;
        break;

    case 'l':
        if(params.count()>=1 && params.contains(1) && extra=="?") { // normal cursor keys
            iAppCursorKeys = false;
        }
        else if(params.count()>=1 && params.contains(3) && extra=="?") { //column mode
            // not supported, just clear screen, move cursor home & reset scrolling region
            clearAll();
            resetTabs();
            iMarginTop = 1;
            iMarginBottom = iTermSize.height();
        }
        else if(params.count()>=1 && params.contains(6) && extra=="?") { //origin mode disable
            iTermAttribs.originMode = false;
        }
        else if(params.count()>=1 && params.contains(7) && extra=="?") { //wraparound mode disable
            iTermAttribs.wrapAroundMode = false;
        }
        else if(params.count()>=1 && params.contains(12) && extra=="?") { // stop blinking cursor
            // no need to do anything, we don't blink
        }
        else if(params.count()>=1 && params.contains(25) && extra=="?") { // hide cursor
            iShowCursor = false;
        }
        else if(params.count()>=1 && params.contains(1049) && extra=="?") { //return from alt screen buffer & restore cursor
            iUseAltScreenBuffer = false;
            iTermAttribs = iTermAttribs_saved_alt;
            iMarginBottom = iTermSize.height();
            iMarginTop = 1;
            resetBackBufferScrollPos();
            resetTabs();
            emit displayBufferChanged();
        }

        else if(params.count()>=1 && params.contains(4) && extra=="") {
            iReplaceMode = false;
        }
        else if(params.count()>=1 && params.contains(20) && extra=="") {
            iNewLineMode = false;
        }
        else unhandled=true;
        break;

    case 'r':  // scrolling region
        if(!extra.isEmpty()) {
            unhandled=true;
            break;
        }
        if(params.count() < 2) {
            while(params.count() < 2)
                params.append(1);
            params[0] = 1;
            params[1] = iTermSize.height();
        }
        if(params.at(0) < 1)
            params[0] = 1;
        if(params.at(1) > iTermSize.height())
            params[1] = iTermSize.height();
        iMarginTop = params.at(0);
        iMarginBottom = params.at(1);
        if(iMarginTop >= iMarginBottom) {
            //invalid scroll region
            if(iMarginTop == iTermSize.height()) {
                iMarginTop = iMarginBottom - 1;
            } else {
                iMarginBottom = iMarginTop + 1;
            }
        }
        setCursorPos(QPoint( 1, iMarginTop ));
        break;

    default:
        unhandled=true;
        break;
    }

    if (unhandled)
        qDebug() << "unhandled ansi sequence " << cmdChar << params << extra;
}

void Terminal::oscSequence(const QString& seq)
{
    if(seq.length() <= 1 || seq.at(0)!=']')
        return;

    // set window title
    if( seq.length() >= 3 && seq.at(0)==']' &&
        (seq.at(1)=='0' || seq.at(1)=='2') &&
        seq.at(2)==';' )
    {
        iUtil->setWindowTitle(seq.mid(3));
        return;
    }

    qDebug() << "unhandled OSC" << seq;
}

void Terminal::escControlChar(const QString& seq)
{
    QChar ch;

    if(seq.length()==1) {
        ch = seq.at(0);
    } else if (seq.length()>1 ){ // control sequences longer than 1 characters
        if( seq.at(0) == '(' || seq.at(0)==')' ) // character set, ignore this for now...
            return;
        if( seq.at(0) == '#' && seq.at(1)=='8' ) { // test mode, fill screen with 'E'
            clearAll(true);
            for(int i=0; i<termSize().height(); i++) {
                QList<TermChar> line;
                for(int j=0; j<termSize().width(); j++) {
                    TermChar c = zeroChar;
                    c.c = 'E';
                    line.append(c);
                }
                buffer().append(line);
            }
            return;
        }
    }

    char latin = ch.toLatin1();

    if(latin=='7') { //save cursor
        iTermAttribs_saved = iTermAttribs;
    }
    else if(latin=='8') { //restore cursor
        iTermAttribs = iTermAttribs_saved;
    }
    else if(latin=='>' || latin=='=') { //app keypad/normal keypad - ignore these for now...
    }

    else if(latin=='H') {  // set a tab stop at cursor position
        while(iTabStops.size() < cursorPos().y())
            iTabStops.append(QList<int>());

        iTabStops[cursorPos().y()-1].append(cursorPos().x());
        qSort(iTabStops[cursorPos().y()-1]);
    }
    else if(latin=='D') {  // cursor down/scroll down one line
        scrollFwd(1, cursorPos().y());
    }
    else if(latin=='M') {  // cursor up/scroll up one line
        scrollBack(1, cursorPos().y());
    }

    else if(latin=='E') {  // new line
        if(cursorPos().y()==iMarginBottom) {
            scrollFwd(1);
            setCursorPos(QPoint(1,cursorPos().y()));
        } else {
            setCursorPos(QPoint(1,cursorPos().y()+1));
        }
    }
    else if(latin=='c') {  // full reset
        resetTerminal();
    }
    else if(latin=='g') {  // visual bell
        iUtil->bellAlert();
    }
    else {
        qDebug() << "unhandled escape code ESC" << seq;
    }
}

QList<TermChar>& Terminal::currentLine()
{
    while(buffer().size() <= cursorPos().y()-1)
        buffer().append(QList<TermChar>());

    if( cursorPos().y() >= 1 &&
            cursorPos().y() <= buffer().size() )
    {
        return buffer()[cursorPos().y()-1];
    }

    // we shouldn't get here
    return buffer()[buffer().size()-1];
}

const QStringList Terminal::printableLinesFromCursor(int lines, bool withEmptyLines)
{
    QStringList ret;

    int start = cursorPos().y() - lines;
    int end = cursorPos().y() + (withEmptyLines ? lines : 0);

    for(int l=start-1; l<end; l++) {
        ret.append("");
        if(l >= 0 && l < buffer().size()) {
            for(int i=0; i<buffer()[l].size(); i++) {
                if(buffer()[l][i].c.isPrint())
                    ret[ret.size()-1].append(buffer()[l][i].c);
            }
        }
    }

    return ret;
}

void Terminal::trimBackBuffer()
{
    while(backBuffer().size() > maxScrollBackLines) {
        backBuffer().removeFirst();
    }
}

void Terminal::scrollBack(int lines, int insertAt)
{
    if(lines <= 0)
        return;

    adjustSelectionPosition(lines);

    bool useBackbuffer = true;
    if(insertAt==-1) {
        insertAt = iMarginTop;
        useBackbuffer = false;
    }
    insertAt--;

    while(lines>0) {
        if(!iUseAltScreenBuffer) {
            if(iBackBuffer.size()>0 && useBackbuffer)
                buffer().insert(insertAt, iBackBuffer.takeLast());
            else
                buffer().insert(insertAt, QList<TermChar>());
        } else {
            buffer().insert(insertAt, QList<TermChar>());
        }

        int rm = iMarginBottom;
        if(rm >= buffer().size())
            rm = buffer().size()-1;

        buffer().removeAt(rm);

        lines--;
    }
}

void Terminal::scrollFwd(int lines, int removeAt)
{
    if(lines <= 0)
        return;

    adjustSelectionPosition(-lines);

    if(removeAt==-1) {
        removeAt = iMarginTop;
    }
    removeAt--;

    while(buffer().size() < iMarginBottom)
        buffer().append(QList<TermChar>());

    while(lines>0) {
        buffer().insert(iMarginBottom, QList<TermChar>());

        if(!iUseAltScreenBuffer)
            iBackBuffer.append( buffer().takeAt(removeAt) );
        else
            buffer().removeAt(removeAt);

        lines--;
    }
    trimBackBuffer();
}

void Terminal::resetTerminal()
{
    iBuffer.clear();
    iAltBuffer.clear();
    iBackBuffer.clear();

    iTermAttribs.currentFgColor = defaultFgColor;
    iTermAttribs.currentBgColor = defaultBgColor;
    iTermAttribs.currentAttrib = 0;
    iTermAttribs.cursorPos = QPoint(1,1);
    iTermAttribs.wrapAroundMode = true;
    iTermAttribs.originMode = false;

    iTermAttribs_saved = iTermAttribs;
    iTermAttribs_saved_alt = iTermAttribs;

    iMarginBottom = iTermSize.height();
    iMarginTop = 1;

    iShowCursor = true;
    iUseAltScreenBuffer = false;
    iAppCursorKeys = false;
    iReplaceMode = false;
    iNewLineMode = false;

    resetBackBufferScrollPos();

    resetTabs();
    clearSelection();
}

void Terminal::resetTabs()
{
    iTabStops.clear();
    for(int i=0; i<iTermSize.height(); i++) {
        int tab=1;
        iTabStops.append(QList<int>());
        while(tab <= iTermSize.width()) {
            iTabStops.last().append(tab);
            tab += 8;
        }
    }
}

void Terminal::pasteFromClipboard()
{
    QClipboard *cb = QGuiApplication::clipboard();

    if(iPtyIFace && !cb->text().isEmpty()) {
        resetBackBufferScrollPos();
        iPtyIFace->writeTerm(cb->text());
    }
}

const QStringList Terminal::grabURLsFromBuffer()
{
    QStringList ret;
    QByteArray buf;

    //backbuffer
    if ((iUtil->settingsValue("gen/grabUrlsFromBackbuffer").toBool()
         && !iUseAltScreenBuffer)
        || backBufferScrollPos() > 0)  //a lazy workaround: just grab everything when the buffer is being scrolled (TODO: make a proper fix)
    {
        for (int i=0; i<iBackBuffer.size(); i++) {
            for (int j=0; j<iBackBuffer[i].size(); j++) {
                if (iBackBuffer[i][j].c.isPrint())
                    buf.append(iBackBuffer[i][j].c);
                else if (iBackBuffer[i][j].c == 0)
                    buf.append(' ');
            }
            if (iBackBuffer[i].size() < iTermSize.width())
                buf.append(' ');
        }
    }

    //main buffer
    for (int i=0; i<buffer().size(); i++) {
        for (int j=0; j<buffer()[i].size(); j++) {
            if (buffer()[i][j].c.isPrint())
                buf.append(buffer()[i][j].c);
            else if (buffer()[i][j].c == 0)
                buf.append(' ');
        }
        if (buffer()[i].size() < iTermSize.width())
            buf.append(' ');
    }

    /* http://blog.mattheworiordan.com/post/13174566389/url-regular-expression-for-links-with-or-without-the */
    QRegularExpression re("("
                   "(" // brackets covering match for protocol (optional) and domain
                     "([A-Za-z]{3,9}:(?:\\/\\/)?)" // match protocol, allow in format http:// or mailto:
                     "(?:[\\-;:&=\\+\\$,\\w]+@)?" // allow something@ for email addresses
                     "[A-Za-z0-9\\.\\-]+" // anything looking at all like a domain, non-unicode domains
                     "|" // or instead of above
                     "(?:www\\.|[\\-;:&=\\+\\$,\\w]+@)" // starting with something@ or www.
                     "[A-Za-z0-9\\.\\-]+"   // anything looking at all like a domain
                   ")"
                   "(" // brackets covering match for path, query string and anchor
                     "(?:\\/[\\+~%\\/\\.\\w\\-]*)" // allow optional /path
                     "?\\?""?(?:[\\-\\+=&;%@\\.\\w]*)" // allow optional query string starting with ?
                     "#?(?:[\\.\\!\\/\\\\\\w]*)" // allow optional anchor #anchor
                   ")?" // make URL suffix optional
               ")");

    QRegularExpressionMatchIterator i = re.globalMatch(buf);
    while (i.hasNext()) {
        QRegularExpressionMatch match = i.next();
        QString word = match.captured(1);
        ret << word;
    }

    ret.removeDuplicates();
    return ret;
}

QString Terminal::getUserMenuXml()
{
    if(!iUtil)
        return QString();

    QString ret;
    QFile f( iUtil->configPath()+"/menu.xml" );
    if(f.open(QIODevice::ReadOnly|QIODevice::Text)) {
        ret = f.readAll();
        f.close();
    }

    return ret;
}

void Terminal::scrollBackBufferFwd(int lines)
{
    if(iUseAltScreenBuffer || lines<=0)
        return;

    clearSelection();

    iBackBufferScrollPos -= lines;
    if(iBackBufferScrollPos < 0)
        iBackBufferScrollPos = 0;

    if (iRenderer) {
        iRenderer->setShowBufferScrollIndicator(iBackBufferScrollPos != 0);
        iRenderer->redraw();
    }
}

void Terminal::scrollBackBufferBack(int lines)
{
    if (iUseAltScreenBuffer || lines<=0)
        return;

    clearSelection();

    iBackBufferScrollPos += lines;
    if (iBackBufferScrollPos > iBackBuffer.size())
        iBackBufferScrollPos = iBackBuffer.size();

    if (iRenderer) {
        iRenderer->setShowBufferScrollIndicator(iBackBufferScrollPos != 0);
        iRenderer->redraw();
    }
}

void Terminal::resetBackBufferScrollPos()
{
    if(iBackBufferScrollPos==0 && iSelection.isNull())
        return;

    iBackBufferScrollPos = 0;
    clearSelection();

    if (iRenderer) {
        iRenderer->setShowBufferScrollIndicator(false);
        iRenderer->redraw();
    }
}

void Terminal::copySelectionToClipboard()
{
    if (selection().isNull())
        return;

    QClipboard *cb = QGuiApplication::clipboard();
    cb->clear();

    QString text;
    QString line;

    // backbuffer
    if (iBackBufferScrollPos > 0 && !iUseAltScreenBuffer) {
        int lineFrom = iBackBuffer.size() - iBackBufferScrollPos + selection().top() - 1;
        int lineTo = iBackBuffer.size() - iBackBufferScrollPos + selection().bottom() - 1;

        for (int i=lineFrom; i<=lineTo; i++) {
            if (i >= 0 && i < iBackBuffer.size()) {
                line.clear();
                int start = 0;
                int end = iBackBuffer[i].size()-1;
                if (i==lineFrom) {
                    start = selection().left()-1;
                }
                if (i==lineTo) {
                    end = selection().right()-1;
                }
                for (int j=start; j<=end; j++) {
                    if (j >= 0 && j < iBackBuffer[i].size() && iBackBuffer[i][j].c.isPrint())
                        line += iBackBuffer[i][j].c;
                }
                text += line.trimmed() + "\n";
            }
        }
    }

    // main buffer
    int lineFrom = selection().top()-1-iBackBufferScrollPos;
    int lineTo = selection().bottom()-1-iBackBufferScrollPos;
    for (int i=lineFrom; i<=lineTo; i++) {
        if (i >= 0 && i < buffer().size()) {
            line.clear();
            int start = 0;
            int end = buffer()[i].size()-1;
            if (i==lineFrom) {
                start = selection().left()-1;
            }
            if (i==lineTo) {
                end = selection().right()-1;
            }
            for (int j=start; j<=end; j++) {
                if (j >= 0 && j < buffer()[i].size() && buffer()[i][j].c.isPrint())
                    line += buffer()[i][j].c;
            }
            text += line.trimmed() + "\n";
        }
    }

    //qDebug() << text.trimmed();

    cb->setText(text.trimmed());
}

void Terminal::adjustSelectionPosition(int lines)
{
    // adjust selection position when terminal contents move

    if (iSelection.isNull() || lines==0)
        return;

    int tx = iSelection.left();
    int ty = iSelection.top() + lines;
    int bx = iSelection.right();
    int by = iSelection.bottom() + lines;

    if (ty<1) {
        ty = 1;
        tx = 1;
    }
    if (by>iTermSize.height()) {
        by = iTermSize.height();
        bx = iTermSize.width();
    }
    if (by<1 || ty>iTermSize.height()) {
        clearSelection();
        return;
    }

    iSelection = QRect(QPoint(tx,ty), QPoint(bx,by));

    if (iRenderer)
        iRenderer->redraw();
}

void Terminal::setSelection(QPoint start, QPoint end)
{
    if (start.y() > end.y())
        qSwap(start, end);
    if (start.y() == end.y() && start.x() > end.x())
        qSwap(start, end);

    if (start.x() < 1)
        start.rx() = 1;
    if (start.y() < 1)
        start.ry() = 1;
    if (end.x() > iTermSize.width())
        end.rx() = iTermSize.width();
    if (end.y() > iTermSize.height())
        end.ry() = iTermSize.height();

    iSelection = QRect(start, end);

    if (iRenderer)
        iRenderer->redraw();
}

void Terminal::clearSelection()
{
    if (iSelection.isNull())
        return;

    iSelection = QRect();

    if (iUtil)
        iUtil->selectionFinished();
    if (iRenderer)
        iRenderer->redraw();
}

QRect Terminal::selection()
{
    return iSelection;
}
