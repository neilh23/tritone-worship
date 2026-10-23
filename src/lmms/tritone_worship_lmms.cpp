#include <QContextMenuEvent>
#include <QDomElement>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <vector>

#include "AutomatableModelView.h"
#include "Effect.h"
#include "EffectControlDialog.h"
#include "EffectControls.h"
#include "Engine.h"
#include "Mixer.h"
#include "plugin_export.h"
#ifdef TW_EQ_TUNING
#include "Knob.h"
#endif

extern "C" {
#include "tw_dsp.h"
#include "tw_ui.h"
}

#ifdef TW_EQ_TUNING
#define EQ_STRIP_H 100
#else
#define EQ_STRIP_H 0
#endif

extern "C" Plugin::Descriptor PLUGIN_EXPORT tritoneworship_plugin_descriptor;

static bool is_switch(int param)
{
    return param == TW_P_BLACKEN || param == TW_P_BYPASS;
}

class TwEffect;
class TwDialog;

class TwControls : public EffectControls
{
public:
    TwControls(TwEffect *effect);

    void saveSettings(QDomDocument &doc, QDomElement &parent) override
    {
        for (int p = 0; p < TW_P_COUNT; p++)
            m_models[p]->saveSettings(doc, parent, tw_param_info[p].symbol);
#ifdef TW_EQ_TUNING
        for (int b = 0; b < TW_EQ_BANDS; b++)
            m_eq[b]->saveSettings(doc, parent, QString("eq_%1").arg(tw_eq_freqs[b]));
#endif
    }

    void loadSettings(const QDomElement &elem) override
    {
        for (int p = 0; p < TW_P_COUNT; p++)
            m_models[p]->loadSettings(elem, tw_param_info[p].symbol);
#ifdef TW_EQ_TUNING
        for (int b = 0; b < TW_EQ_BANDS; b++)
            m_eq[b]->loadSettings(elem, QString("eq_%1").arg(tw_eq_freqs[b]));
#endif
    }

    QString nodeName() const override { return "tritoneworshipcontrols"; }
    int controlCount() override { return TW_P_COUNT; }
    EffectControlDialog *createView() override;

    AutomatableModel *m_models[TW_P_COUNT];
#ifdef TW_EQ_TUNING
    FloatModel *m_eq[TW_EQ_BANDS];
#endif
};

class TwEffect : public Effect
{
public:
    TwEffect(Model *parent, const Descriptor::SubPluginFeatures::Key *key) :
        Effect(&tritoneworship_plugin_descriptor, parent, key),
        m_controls(this)
    {
    }

    ~TwEffect() override { tw_destroy(m_dsp); }

    EffectControls *controls() override { return &m_controls; }

    bool processAudioBuffer(sampleFrame *buf, const fpp_t frames) override
    {
        if (!isEnabled() || !isRunning())
            return false;

        float sr = Engine::mixer()->processingSampleRate();
        if (!m_dsp || sr != m_sr) {
            tw_destroy(m_dsp);
            m_dsp = tw_create(sr);
            m_sr = sr;
        }
        for (int p = 0; p < TW_P_COUNT; p++)
            tw_set_param(&m_dsp->p, p, m_controls.m_models[p]->value<float>());
#ifdef TW_EQ_TUNING
        for (int b = 0; b < TW_EQ_BANDS; b++)
            m_dsp->p.eq_db[b] = m_controls.m_eq[b]->value();
#endif

        m_l.resize(frames);
        m_r.resize(frames);
        for (fpp_t f = 0; f < frames; f++) {
            m_l[f] = buf[f][0];
            m_r[f] = buf[f][1];
        }
        tw_process(m_dsp, m_l.data(), m_r.data(), m_l.data(), m_r.data(), frames);

        const float d = dryLevel(), w = wetLevel();
        double outSum = 0.0;
        for (fpp_t f = 0; f < frames; f++) {
            buf[f][0] = d * buf[f][0] + w * m_l[f];
            buf[f][1] = d * buf[f][1] + w * m_r[f];
            outSum += buf[f][0] * buf[f][0] + buf[f][1] * buf[f][1];
        }
        checkGate(outSum / frames);
        return isRunning();
    }

private:
    TwControls m_controls;
    TwState *m_dsp = nullptr;
    float m_sr = 0;
    std::vector<float> m_l, m_r;
};

TwControls::TwControls(TwEffect *effect) :
    EffectControls(effect)
{
    for (int p = 0; p < TW_P_COUNT; p++) {
        const TwParamInfo *i = &tw_param_info[p];
        if (is_switch(p))
            m_models[p] = new BoolModel(i->def > 0.5f, this, i->name);
        else
            m_models[p] = new FloatModel(i->def, i->min, i->max, 0.1f, this, i->name);
    }
#ifdef TW_EQ_TUNING
    for (int b = 0; b < TW_EQ_BANDS; b++)
        m_eq[b] = new FloatModel(tw_blacken_default_db[b], -24, 24, 0.1f, this,
                                 QString("EQ %1 Hz").arg(tw_eq_freqs[b]));
#endif
}

class TwDialog : public EffectControlDialog
{
public:
    TwDialog(TwControls *controls);

    void setActive(int param)
    {
        m_active = param;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        float v[TW_P_COUNT];
        for (int p = 0; p < TW_P_COUNT; p++)
            v[p] = m_controls->m_models[p]->value<float>();
        tw_ui_draw(reinterpret_cast<uint32_t *>(m_image.bits()), v, m_active);
        QPainter painter(this);
        painter.drawImage(0, 0, m_image);
        if (EQ_STRIP_H)
            painter.fillRect(0, TW_UI_H, TW_UI_W, EQ_STRIP_H, QColor(0x26, 0x28, 0x2b));
    }

private:
    TwControls *m_controls;
    QImage m_image;
    int m_active = -1;
};

/* Invisible control over a knob or the footswitch of the drawn pedal. Being an
   AutomatableModelView gives LMMS's automation menu, ctrl-drag and middle-click
   reset. */
class TwControl : public QWidget, public AutomatableModelView
{
public:
    TwControl(AutomatableModel *model, int param, TwDialog *dialog) :
        QWidget(dialog),
        AutomatableModelView(model, this),
        m_param(param),
        m_dialog(dialog)
    {
        TwUiRect r = tw_ui_rect(param);
        setGeometry(r.x, r.y, r.w, r.h);
        setToolTip(tw_param_info[param].name);
        setCursor(is_switch(param) ? Qt::PointingHandCursor : Qt::SizeVerCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        AutomatableModelView::mousePressEvent(e);
        if (e->button() != Qt::LeftButton || (e->modifiers() & Qt::ControlModifier))
            return;
        AutomatableModel *m = modelUntyped();
        if (is_switch(m_param)) {
            m->setValue(m->value<float>() > 0.5f ? 0.0f : 1.0f);
            return;
        }
        m->addJournalCheckPoint();
        m_dragging = true;
        m_dragY = e->y();
        m_dragStart = m->value<float>();
        m_dialog->setActive(m_param);
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging)
            modelUntyped()->setValue(tw_ui_drag_value(m_param, m_dragStart, m_dragY - e->y()));
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (!m_dragging)
            return;
        m_dragging = false;
        m_dialog->setActive(-1);
    }

    void contextMenuEvent(QContextMenuEvent *e) override
    {
        QMenu menu(this);
        addDefaultActions(&menu);
        menu.exec(e->globalPos());
    }

private:
    int m_param;
    TwDialog *m_dialog;
    bool m_dragging = false;
    int m_dragY = 0;
    float m_dragStart = 0;
};

TwDialog::TwDialog(TwControls *controls) :
    EffectControlDialog(controls),
    m_controls(controls),
    m_image(TW_UI_W, TW_UI_H, QImage::Format_RGB32)
{
    setFixedSize(TW_UI_W, TW_UI_H + EQ_STRIP_H);
    for (int p = 0; p < TW_P_COUNT; p++) {
        new TwControl(controls->m_models[p], p, this);
        /* Redraw the whole pedal on every change: drags, automation, undo */
        connect(controls->m_models[p], &Model::dataChanged, this, [this] { update(); });
    }
#ifdef TW_EQ_TUNING
    for (int b = 0; b < TW_EQ_BANDS; b++) {
        float f = tw_eq_freqs[b];
        Knob *k = new Knob(knobSmall_17, this);
        k->move(6 + (b % 10) * 27, TW_UI_H + 6 + (b / 10) * 46);
        k->setModel(controls->m_eq[b]);
        k->setLabel(f >= 1000 ? QString("%1k").arg(f / 1000) : QString::number(f));
        k->setHintText(QString("EQ %1 Hz:").arg(f), " dB");
    }
#endif
}

EffectControlDialog *TwControls::createView()
{
    return new TwDialog(this);
}

extern "C" {

Plugin::Descriptor PLUGIN_EXPORT tritoneworship_plugin_descriptor = {
    "tritoneworship",
    "TritoneWorship",
    "Minor third stacking, death octave, black metal EQ and tremolo picking",
    "Neil Howie",
    0x0100,
    Plugin::Effect,
    nullptr,
    nullptr,
    nullptr,
};

PLUGIN_EXPORT Plugin *lmms_plugin_main(Model *parent, void *data)
{
    return new TwEffect(parent, static_cast<const Plugin::Descriptor::SubPluginFeatures::Key *>(data));
}

}
