#include "SdCardWindow.h"
#include "Storage.h"

namespace
{
    juce::Colour bg() { return juce::Colour (0xff1c1d20); }
    juce::Colour ink() { return juce::Colour (0xffd8d8d0); }
    juce::Colour accent() { return juce::Colour (0xffff5a1f); }

    juce::String bytes (uint64_t n)
    {
        if (n >= (1ull << 30))
            return juce::String ((double) n / (1ull << 30), 1) + " GB";
        if (n >= (1ull << 20))
            return juce::String ((double) n / (1ull << 20), 1) + " MB";
        if (n >= 1024)
            return juce::String ((int) (n / 1024)) + " KB";
        return juce::String ((int) n) + " B";
    }

    juce::String utf8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

    // Work on a thread behind a progress window; `done` hears the result
    // on the message thread. Deletes itself.
    class Job : public juce::ThreadWithProgressWindow
    {
    public:
        Job (const juce::String& title, std::function<juce::String (Job&)> w, std::function<void (const juce::String&)> d)
            : juce::ThreadWithProgressWindow (title, true, true), work (std::move (w)), done (std::move (d))
        {
            setProgress (-1.0);
        }
        void run() override { result = work (*this); }
        void threadComplete (bool cancelled) override
        {
            done (cancelled && result.isEmpty() ? juce::String ("Stopped.") : result);
            delete this;
        }

    private:
        std::function<juce::String (Job&)> work;
        std::function<void (const juce::String&)> done;
        juce::String result;
    };

    void showError (const juce::String& title, const juce::String& text)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, text);
    }
}

//==============================================================================
SdSlot::SdSlot (EmulatorLink& l, juce::PropertiesFile& s) : link (l), settings (s)
{
    inserted = settings.getBoolValue ("sdInserted", true);
}

void SdSlot::setInserted (bool in, std::function<void (const juce::String&)> done)
{
    if (! link.isConnected())
    {
        inserted = in;
        settings.setValue ("sdInserted", in);
        settings.saveIfNeeded();
        if (onChange)
            onChange();
        if (done)
            done ({});
        return;
    }
    pending = std::move (done);
    link.sendSdCard (in ? Storage::sdCard() : juce::File());
}

void SdSlot::handleReply (bool nowInserted, const juce::String& error)
{
    inserted = nowInserted;
    settings.setValue ("sdInserted", nowInserted);
    settings.saveIfNeeded();
    if (onChange)
        onChange();
    if (auto p = std::move (pending))
        p (error);
}

//==============================================================================
SdCardComponent::SdCardComponent (SdSlot& s, juce::File img) : slot (s), image (std::move (img))
{
    for (auto* l : { &status, &space, &path, &hint })
    {
        addAndMakeVisible (*l);
        l->setColour (juce::Label::textColourId, ink());
    }
    status.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    space.setJustificationType (juce::Justification::centredRight);
    hint.setColour (juce::Label::textColourId, ink().withAlpha (0.6f));
    hint.setText ("Drag files or folders here to copy them onto the card; drag items out to copy them off. "
                  "The SP-404 imports from IMPORT and exports to EXPORT.",
                  juce::dontSendNotification);

    for (auto* b : { &slotButton, &upButton, &addButton, &folderButton, &exportButton, &deleteButton, &formatButton })
        addAndMakeVisible (*b);
    slotButton.onClick = [this]
    {
        const bool in = ! slot.isInserted();
        slot.setInserted (in, [this, in] (const juce::String& e)
        {
            if (e.isNotEmpty())
                showError ("SD card", e);
            else
                tookCard = ! in;
            refresh();
        });
    };
    upButton.onClick = [this] { open (cwd.upToLastOccurrenceOf ("/", false, false)); };
    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Copy onto the SD card", juce::File(), "*");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc)
                              {
                                  juce::StringArray files;
                                  for (auto& f : fc.getResults())
                                      files.add (f.getFullPathName());
                                  if (! files.isEmpty())
                                      addFiles (files);
                              });
    };
    folderButton.onClick = [this] { newFolder(); };
    exportButton.onClick = [this] { exportSelected(); };
    deleteButton.onClick = [this] { deleteSelected(); };
    formatButton.onClick = [this] { formatCard(); };

    addAndMakeVisible (table);
    auto& h = table.getHeader();
    h.addColumn ("Name", 1, 320, 100, -1, juce::TableHeaderComponent::defaultFlags);
    h.addColumn ("Size", 2, 90, 60, -1, juce::TableHeaderComponent::defaultFlags);
    h.addColumn ("Modified", 3, 140, 80, -1, juce::TableHeaderComponent::defaultFlags);
    h.setSortColumnId (1, true);
    h.setColour (juce::TableHeaderComponent::backgroundColourId, juce::Colour (0xff2c2e33));
    h.setColour (juce::TableHeaderComponent::textColourId, ink());
    h.setColour (juce::TableHeaderComponent::outlineColourId, juce::Colour (0xff15161a));
    table.setMultipleSelectionEnabled (true);
    table.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff15161a));
    table.setRowHeight (24);

    slot.onChange = [this] { refresh(); };
    setSize (640, 520);
    refresh();
}

SdCardComponent::~SdCardComponent()
{
    slot.onChange = nullptr;
    if (dragTemp != juce::File())
        dragTemp.deleteRecursively();
}

void SdCardComponent::returnCard()
{
    if (tookCard && ! slot.isInserted())
        slot.setInserted (true);
    tookCard = false;
}

void SdCardComponent::paint (juce::Graphics& g)
{
    g.fillAll (bg());
    if (dropping)
    {
        g.setColour (accent());
        g.drawRect (table.getBounds().expanded (2), 3);
    }
}

void SdCardComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto top = r.removeFromTop (30);
    slotButton.setBounds (top.removeFromRight (170));
    space.setBounds (top.removeFromRight (180));
    status.setBounds (top);
    r.removeFromTop (6);
    auto nav = r.removeFromTop (26);
    upButton.setBounds (nav.removeFromLeft (50));
    nav.removeFromLeft (8);
    path.setBounds (nav);
    r.removeFromTop (6);
    hint.setBounds (r.removeFromBottom (36));
    auto buttons = r.removeFromBottom (30);
    r.removeFromBottom (6);
    table.setBounds (r);
    const int w = buttons.getWidth() / 5;
    for (auto* b : { &addButton, &folderButton, &exportButton, &deleteButton, &formatButton })
        b->setBounds (buttons.removeFromLeft (w).reduced (2, 0));
}

void SdCardComponent::open (const juce::String& dir)
{
    cwd = dir.isEmpty() ? juce::String ("/") : dir;
    table.deselectAllRows();
    refresh();
}

void SdCardComponent::refresh()
{
    const bool in = slot.isInserted();
    status.setText (in ? "In the SP-404" : "Out of the SP-404", juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, in ? ink() : accent());
    slotButton.setButtonText (in ? "Take out of the unit" : "Put back in the unit");

    entries.clear();
    fatimg::Volume v;
    auto e = v.open (Storage::toPath (image), true);
    if (e.empty())
    {
        e = v.list (cwd.toStdString(), entries);
        if (! e.empty() && cwd != "/")
        {
            cwd = "/";
            e = v.list ("/", entries);
        }
        entries.erase (std::remove_if (entries.begin(), entries.end(), [] (auto& x) { return x.hidden; }), entries.end());
        space.setText (bytes (v.freeBytes()) + " free of " + bytes (v.totalBytes()), juce::dontSendNotification);
    }
    else
        space.setText ("", juce::dontSendNotification);
    v.close();
    path.setText (e.empty() ? "SD:" + cwd : "Cannot read the card: " + utf8 (e), juce::dontSendNotification);
    sortOrderChanged (sortColumn, sortForwards);
    upButton.setEnabled (cwd != "/");
    updateButtons();
}

void SdCardComponent::sortOrderChanged (int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    std::stable_sort (entries.begin(), entries.end(), [&] (const fatimg::Entry& a, const fatimg::Entry& b)
    {
        if (a.dir != b.dir)
            return a.dir;
        int c = 0;
        if (column == 2)
            c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
        else if (column == 3)
        {
            auto key = [] (const fatimg::Entry& x) { return (((x.year * 13 + x.month) * 32 + x.day) * 24 + x.hour) * 60 + x.minute; };
            c = key (a) - key (b);
        }
        if (c == 0)
            c = utf8 (a.name).compareNatural (utf8 (b.name));
        return forwards ? c < 0 : c > 0;
    });
    table.updateContent();
    table.repaint();
}

void SdCardComponent::updateButtons()
{
    const bool any = table.getNumSelectedRows() > 0;
    exportButton.setEnabled (any);
    deleteButton.setEnabled (any);
}

void SdCardComponent::paintRowBackground (juce::Graphics& g, int, int, int, bool selected)
{
    if (selected)
        g.fillAll (accent().withAlpha (0.35f));
}

void SdCardComponent::paintCell (juce::Graphics& g, int row, int column, int w, int h, bool)
{
    if (row < 0 || row >= (int) entries.size())
        return;
    const auto& e = entries[(size_t) row];
    g.setColour (ink());
    g.setFont (juce::FontOptions (14.0f));
    juce::String text;
    if (column == 1)
        text = (e.dir ? juce::String::fromUTF8 ("\xf0\x9f\x93\x81 ") : juce::String()) + utf8 (e.name);
    else if (column == 2)
        text = e.dir ? juce::String() : bytes (e.size);
    else
        text = juce::String::formatted ("%04d-%02d-%02d %02d:%02d", e.year, e.month, e.day, e.hour, e.minute);
    g.drawText (text, 6, 0, w - 8, h, column == 2 ? juce::Justification::centredRight : juce::Justification::centredLeft);
}

void SdCardComponent::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    returnKeyPressed (row);
}

void SdCardComponent::returnKeyPressed (int row)
{
    if (row >= 0 && row < (int) entries.size() && entries[(size_t) row].dir)
        open (utf8 (fatimg::join (cwd.toStdString(), entries[(size_t) row].name)));
}

std::vector<std::string> SdCardComponent::selectedPaths() const
{
    std::vector<std::string> out;
    for (int i = 0; i < table.getNumSelectedRows(); ++i)
        if (const int row = table.getSelectedRow (i); row >= 0 && row < (int) entries.size())
            out.push_back (fatimg::join (cwd.toStdString(), entries[(size_t) row].name));
    return out;
}

void SdCardComponent::change (const juce::String& title,
                              std::function<juce::String (fatimg::Volume&, juce::ThreadWithProgressWindow&)> job)
{
    auto run = [this, title, job]
    {
        auto file = Storage::toPath (image);
        juce::Component::SafePointer<SdCardComponent> safe (this);
        (new Job (title, [file, job] (Job& j)
        {
            fatimg::Volume v;
            if (auto e = v.open (file, false); ! e.empty())
                return "Cannot open the card: " + utf8 (e);
            auto e = job (v, j);
            v.close();
            return e;
        },
        [safe, title] (const juce::String& e)
        {
            if (e.isNotEmpty())
                showError (title, e);
            if (safe != nullptr)
                safe->refresh();
        }))->launchThread();
    };
    if (! slot.isInserted())
    {
        run();
        return;
    }
    // Like pulling the card: the unit shows "No SD Card" until it is back.
    slot.setInserted (false, [this, run] (const juce::String& e)
    {
        if (e.isNotEmpty())
            return showError ("SD card", "Could not take the card out of the unit: " + e);
        tookCard = true;
        run();
    });
}

void SdCardComponent::addFiles (const juce::StringArray& files)
{
    const auto dir = cwd.toStdString();
    change ("Copying onto the SD card", [files, dir] (fatimg::Volume& v, juce::ThreadWithProgressWindow& j)
    {
        for (auto& f : files)
        {
            auto e = v.add (Storage::toPath (juce::File (f)), dir, [&j] (const std::string& p)
            {
                j.setStatusMessage (utf8 (p));
                return ! j.threadShouldExit();
            });
            if (! e.empty())
                return utf8 (e);
        }
        return juce::String();
    });
}

void SdCardComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dropping = false;
    repaint();
    addFiles (files);
}

void SdCardComponent::exportSelected()
{
    const auto paths = selectedPaths();
    if (paths.empty())
        return;
    chooser = std::make_unique<juce::FileChooser> ("Export to folder",
                                                   juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, paths] (const juce::FileChooser& fc)
    {
        const auto dest = fc.getResult();
        if (dest == juce::File())
            return;
        // Reading needs no eject: the image is only read.
        auto file = Storage::toPath (image);
        (new Job ("Exporting from the SD card", [file, paths, dest] (Job& j)
        {
            fatimg::Volume v;
            if (auto e = v.open (file, true); ! e.empty())
                return "Cannot open the card: " + utf8 (e);
            for (auto& p : paths)
                if (auto e = v.extract (p, Storage::toPath (dest), [&j] (const std::string& s)
                    {
                        j.setStatusMessage (utf8 (s));
                        return ! j.threadShouldExit();
                    });
                    ! e.empty())
                    return utf8 (e);
            dest.revealToUser();
            return juce::String();
        },
        [] (const juce::String& e)
        {
            if (e.isNotEmpty())
                showError ("Export", e);
        }))->launchThread();
    });
}

bool SdCardComponent::shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&,
                                                             juce::StringArray& files, bool& canMoveFiles)
{
    // Dragged out of the window: copy the selection to a temporary folder
    // and hand the OS those files.
    const auto paths = selectedPaths();
    if (paths.empty())
        return false;
    if (dragTemp != juce::File())
        dragTemp.deleteRecursively();
    dragTemp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("DM-404 SD " + juce::String (juce::Time::currentTimeMillis()));
    dragTemp.createDirectory();
    fatimg::Volume v;
    if (! v.open (Storage::toPath (image), true).empty())
        return false;
    for (auto& p : paths)
        if (v.extract (p, Storage::toPath (dragTemp)).empty())
            files.add (dragTemp.getChildFile (utf8 (p).fromLastOccurrenceOf ("/", false, false)).getFullPathName());
    canMoveFiles = false;
    return ! files.isEmpty();
}

void SdCardComponent::deleteSelected()
{
    const auto paths = selectedPaths();
    if (paths.empty())
        return;
    const auto what = paths.size() == 1 ? "\"" + utf8 (paths[0]).fromLastOccurrenceOf ("/", false, false) + "\""
                                        : juce::String ((int) paths.size()) + " items";
    auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Delete",
                                                              "Delete " + what + " from the SD card?", "Delete", "Cancel", this);
    juce::AlertWindow::showAsync (opts, [this, paths] (int result)
    {
        if (result != 1)
            return;
        change ("Deleting", [paths] (fatimg::Volume& v, juce::ThreadWithProgressWindow& j)
        {
            for (auto& p : paths)
            {
                j.setStatusMessage (utf8 (p));
                if (auto e = v.remove (p); ! e.empty())
                    return utf8 (e);
            }
            return juce::String();
        });
    });
}

void SdCardComponent::newFolder()
{
    auto* w = new juce::AlertWindow ("New folder", "Name:", juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", "NEW FOLDER");
    w->addButton ("Make", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([this, w] (int result)
    {
        const auto name = w->getTextEditorContents ("name").trim();
        if (result != 1 || name.isEmpty())
            return;
        const auto p = fatimg::join (cwd.toStdString(), name.toStdString());
        change ("New folder", [p] (fatimg::Volume& v, juce::ThreadWithProgressWindow&) { return utf8 (v.makeDir (p)); });
    }), true);
}

void SdCardComponent::formatCard()
{
    auto opts = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Format card",
                                                              "Erase everything on the SD card?", "Erase", "Cancel", this);
    juce::AlertWindow::showAsync (opts, [this] (int result)
    {
        if (result != 1)
            return;
        auto doFormat = [this]
        {
            const auto size = juce::jmax (image.getSize(), Storage::defaultSdBytes);
            if (auto e = Storage::formatSdCard (image, size); e.isNotEmpty())
                showError ("Format card", e);
            cwd = "/";
            refresh();
        };
        if (! slot.isInserted())
            return doFormat();
        slot.setInserted (false, [this, doFormat] (const juce::String& e)
        {
            if (e.isNotEmpty())
                return showError ("SD card", e);
            tookCard = true;
            doFormat();
        });
    });
}

//==============================================================================
SdCardWindow::SdCardWindow (SdSlot& slot, juce::File image, std::function<void()> onClose)
    : juce::DocumentWindow ("SD card", juce::Colour (0xff1c1d20), juce::DocumentWindow::closeButton),
      closed (std::move (onClose))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new SdCardComponent (slot, std::move (image)), true);
    setResizable (true, false);
    setResizeLimits (480, 360, 4000, 4000);
}

void SdCardWindow::activeWindowStatusChanged()
{
    // Back from elsewhere: the unit may have written to the card meanwhile.
    if (isActiveWindow())
        content().refresh();
}

void SdCardWindow::closeButtonPressed()
{
    content().returnCard();
    if (closed)
        closed();
}
