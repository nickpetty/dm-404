#pragma once

#include "EmulatorLink.h"
#include "FatImage.h"

// The unit's SD slot: whether the card is in, remembered between runs, and
// taking it out or putting it back while the emulator runs (the firmware
// sees its card-detect switch change, as when a card is pulled).
class SdSlot
{
public:
    SdSlot (EmulatorLink&, juce::PropertiesFile&);

    bool isInserted() const { return inserted; }
    // `done` gets an error, empty if it worked. Without a running emulator
    // only the remembered state changes (it applies at the next start).
    void setInserted (bool in, std::function<void (const juce::String&)> done = {});
    // The emulator's answer (from EmulatorLink::onSdCard, message thread).
    void handleReply (bool nowInserted, const juce::String& error);

    std::function<void()> onChange;

private:
    EmulatorLink& link;
    juce::PropertiesFile& settings;
    bool inserted = true;
    std::function<void (const juce::String&)> pending;
};

// Its contents: browse the card image, drag files in from the desktop or
// out to it, export, delete, make folders, format. Changing the card takes
// it out of the unit first; closing the window puts it back.
class SdCardComponent : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        public juce::DragAndDropContainer,
                        private juce::TableListBoxModel
{
public:
    SdCardComponent (SdSlot&, juce::File image);
    ~SdCardComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    // Puts the card back if this window took it out.
    void returnCard();

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void fileDragEnter (const juce::StringArray&, int, int) override { dropping = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dropping = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails&,
                                                juce::StringArray& files, bool& canMoveFiles) override;

private:
    int getNumRows() override { return (int) entries.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int w, int h, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    void deleteKeyPressed (int) override { deleteSelected(); }
    void selectedRowsChanged (int) override { updateButtons(); }
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override { return "sdcard"; }
    void sortOrderChanged (int column, bool forwards) override;

    void open (const juce::String& dir);
    void updateButtons();
    std::vector<std::string> selectedPaths() const;
    // Runs `job` with the card out of the unit, on a thread with a progress
    // window, then refreshes. `job` returns an error or empty.
    void change (const juce::String& title, std::function<juce::String (fatimg::Volume&, juce::ThreadWithProgressWindow&)> job);
    void addFiles (const juce::StringArray& files);
    void exportSelected();
    void deleteSelected();
    void newFolder();
    void formatCard();

    SdSlot& slot;
    juce::File image;
    juce::String cwd = "/";
    std::vector<fatimg::Entry> entries;
    bool tookCard = false, dropping = false;
    int sortColumn = 1;
    bool sortForwards = true;
    juce::File dragTemp;

    juce::Label status, space, path, hint;
    juce::TextButton slotButton, upButton { "Up" }, addButton { "Add files..." }, folderButton { "New folder" },
        exportButton { "Export..." }, deleteButton { "Delete" }, formatButton { "Format card..." };
    juce::TableListBox table { "SD card", this };
    std::unique_ptr<juce::FileChooser> chooser;
};

class SdCardWindow : public juce::DocumentWindow
{
public:
    SdCardWindow (SdSlot&, juce::File image, std::function<void()> onClose);
    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;
    SdCardComponent& content() { return *static_cast<SdCardComponent*> (getContentComponent()); }

private:
    std::function<void()> closed;
};
