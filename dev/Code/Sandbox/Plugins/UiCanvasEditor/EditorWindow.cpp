/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/
#include "stdafx.h"

#include "EditorCommon.h"
#include <AzCore/IO/SystemFile.h>
#include <AzCore/std/sort.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzQtComponents/Components/StyledDockWidget.h>
#include <LyShine/UiComponentTypes.h>
#include <Util/PathUtil.h>
#include <LyMetricsProducer/LyMetricsAPI.h>
#include "EditorDefs.h"
#include "Settings.h"
#include "AnchorPresets.h"
#include "PivotPresets.h"
#include "Animation/UiAnimViewDialog.h"
#include "AssetTreeEntry.h"

#define UICANVASEDITOR_SETTINGS_EDIT_MODE_STATE_KEY     (QString("Edit Mode State") + " " + FileHelpers::GetAbsoluteGameDir())
#define UICANVASEDITOR_SETTINGS_EDIT_MODE_GEOM_KEY      (QString("Edit Mode Geometry") + " " + FileHelpers::GetAbsoluteGameDir())
#define UICANVASEDITOR_SETTINGS_PREVIEW_MODE_STATE_KEY  (QString("Preview Mode State") + " " + FileHelpers::GetAbsoluteGameDir())
#define UICANVASEDITOR_SETTINGS_PREVIEW_MODE_GEOM_KEY   (QString("Preview Mode Geometry") + " " + FileHelpers::GetAbsoluteGameDir())
#define UICANVASEDITOR_SETTINGS_WINDOW_STATE_VERSION    (1)

namespace
{
    //! \brief Writes the current value of the sys_localization_folder CVar to the editor settings file (Amazon.ini)
    void SaveStartupLocalizationFolderSetting()
    {
        if (gEnv && gEnv->pConsole)
        {
            ICVar* locFolderCvar = gEnv->pConsole->GetCVar("sys_localization_folder");

            QSettings settings(QSettings::IniFormat, QSettings::UserScope, AZ_QCOREAPPLICATION_SETTINGS_ORGANIZATION_NAME);
            settings.beginGroup(UICANVASEDITOR_NAME_SHORT);

            settings.setValue(UICANVASEDITOR_SETTINGS_STARTUP_LOC_FOLDER_KEY, locFolderCvar->GetString());

            settings.endGroup();
            settings.sync();
        }
    }

    //! \brief Reads loc folder value from Amazon.ini and re-sets the CVar accordingly
    void RestoreStartupLocalizationFolderSetting()
    {
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, AZ_QCOREAPPLICATION_SETTINGS_ORGANIZATION_NAME);
        settings.beginGroup(UICANVASEDITOR_NAME_SHORT);

        QString startupLocFolder(settings.value(UICANVASEDITOR_SETTINGS_STARTUP_LOC_FOLDER_KEY).toString());
        if (!startupLocFolder.isEmpty() && gEnv && gEnv->pConsole)
        {
            ICVar* locFolderCvar = gEnv->pConsole->GetCVar("sys_localization_folder");
            locFolderCvar->Set(startupLocFolder.toUtf8().constData());
        }

        settings.endGroup();
        settings.sync();
    }
}

EditorWindow::UiCanvasEditState::UiCanvasEditState()
    : m_inited(false)
{
}

EditorWindow::UiCanvasMetadata::UiCanvasMetadata()
    : m_entityContext(nullptr)
    , m_undoStack(nullptr)
    , m_canvasChangedAndSaved(false)
{
}

EditorWindow::UiCanvasMetadata::~UiCanvasMetadata()
{
    delete m_entityContext;
    delete m_undoStack;
}

EditorWindow::EditorWindow(QWidget* parent, Qt::WindowFlags flags)
    : QMainWindow(parent, flags)
    , IEditorNotifyListener()
    , m_undoGroup(new QUndoGroup(this))
    , m_sliceManager(new UiSliceManager(AzFramework::EntityContextId::CreateNull()))
    , m_hierarchy(new HierarchyWidget(this))
    , m_properties(new PropertiesWrapper(m_hierarchy, this))
    , m_canvasTabBar(nullptr)
    , m_canvasTabSectionWidget(nullptr)
    , m_viewport(nullptr)
    , m_animationWidget(new CUiAnimViewDialog(this))
    , m_previewActionLog(new PreviewActionLog(this))
    , m_previewAnimationList(new PreviewAnimationList(this))
    , m_mainToolbar(new MainToolbar(this))
    , m_modeToolbar(new ModeToolbar(this))
    , m_enterPreviewToolbar(new EnterPreviewToolbar(this))
    , m_previewToolbar(new PreviewToolbar(this))
    , m_hierarchyDockWidget(nullptr)
    , m_propertiesDockWidget(nullptr)
    , m_animationDockWidget(nullptr)
    , m_previewActionLogDockWidget(nullptr)
    , m_previewAnimationListDockWidget(nullptr)
    , m_editorMode(UiEditorMode::Edit)
    , m_prefabFiles()
    , m_actionsEnabledWithSelection()
    , m_pasteAsSiblingAction(nullptr)
    , m_pasteAsChildAction(nullptr)
    , m_previewModeCanvasEntityId()
    , m_previewModeCanvasSize(0.0f, 0.0f)
    , m_clipboardConnection()
    , m_newCanvasCount(1)
{
    // Since the lifetime of EditorWindow and the UI Editor itself aren't the
    // same, we use the initial opening of the UI Editor to save the current
    // value of the loc folder CVar since the user can temporarily change its
    // value while using the UI Editor.
    SaveStartupLocalizationFolderSetting();

    PropertyHandlers::Register();

    // Store local copy of startup localization value
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, AZ_QCOREAPPLICATION_SETTINGS_ORGANIZATION_NAME);
    settings.beginGroup(UICANVASEDITOR_NAME_SHORT);
    m_startupLocFolderName = settings.value(UICANVASEDITOR_SETTINGS_STARTUP_LOC_FOLDER_KEY).toString();
    settings.endGroup();

    // update menus when the selection changes
    connect(m_hierarchy, &HierarchyWidget::SetUserSelection, this, &EditorWindow::UpdateActionsEnabledState);
    m_clipboardConnection = connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &EditorWindow::UpdateActionsEnabledState);

    UpdatePrefabFiles();

    // disable rendering of the editor window until we have restored the window state
    setUpdatesEnabled(false);

    // Create the central widget
    QWidget* centralWidget = new QWidget(this);

    // Create a vertical layout for the central widget that will lay out a tab section widget and a viewport widget
    QVBoxLayout* centralWidgetLayout = new QVBoxLayout(centralWidget);
    centralWidgetLayout->setContentsMargins(0, 0, 0, 0);
    centralWidgetLayout->setSpacing(0);

    // Create a tab section widget that's a child of the central widget
    m_canvasTabSectionWidget = new QWidget(centralWidget);
    m_canvasTabSectionWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    // Add the tab section widget to the layout of the central widget
    centralWidgetLayout->addWidget(m_canvasTabSectionWidget);

    // Create a horizontal layout for the tab section widget that will lay out a tab bar and an add canvas button
    QHBoxLayout* canvasTabSectionWidgetLayout = new QHBoxLayout(m_canvasTabSectionWidget);
    canvasTabSectionWidgetLayout->setContentsMargins(0, 0, 0, 0);

    // Create a canvas tab bar that's a child of the tab section widget
    m_canvasTabBar = new QTabBar(m_canvasTabSectionWidget);
    m_canvasTabBar->setMovable(true);
    m_canvasTabBar->setTabsClosable(true);
    m_canvasTabBar->setExpanding(false);
    m_canvasTabBar->setDocumentMode(true);
    m_canvasTabBar->setDrawBase(false);
    m_canvasTabBar->setContextMenuPolicy(Qt::CustomContextMenu);

    // Add the canvas tab bar to the layout of the tab section widget
    canvasTabSectionWidgetLayout->addWidget(m_canvasTabBar);

    // Create a "add canvas" button  that's a child of the tab section widget
    const int addCanvasButtonPadding = 3;
    QPushButton* addCanvasButton = new QPushButton(tr("+"), m_canvasTabSectionWidget);
    // Get the height of the tab bar to determine the button size
    m_canvasTabBar->addTab("Temp");
    int tabBarHeight = m_canvasTabBar->sizeHint().height();
    m_canvasTabBar->removeTab(0);
    int addCanvasButtonSize = tabBarHeight - (addCanvasButtonPadding * 2);
    addCanvasButton->setFixedSize(addCanvasButtonSize, addCanvasButtonSize);
    addCanvasButton->setToolTip(tr("New Canvas (Ctrl+N)"));
    QObject::connect(addCanvasButton, &QPushButton::clicked, this, [this] { NewCanvas();  });
    QHBoxLayout* addCanvasButtonLayout = new QHBoxLayout();
    addCanvasButtonLayout->setContentsMargins(0, addCanvasButtonPadding, addCanvasButtonPadding, addCanvasButtonPadding);
    addCanvasButtonLayout->addWidget(addCanvasButton);

    // Add the "add canvas" button to the layout of the tab section widget
    canvasTabSectionWidgetLayout->addLayout(addCanvasButtonLayout);

    connect(m_canvasTabBar, &QTabBar::tabCloseRequested, this, &EditorWindow::OnCanvasTabCloseButtonPressed);
    connect(m_canvasTabBar, &QTabBar::currentChanged, this, &EditorWindow::OnCurrentCanvasTabChanged);
    connect(m_canvasTabBar, &QTabBar::customContextMenuRequested, this, &EditorWindow::OnCanvasTabContextMenuRequested);

    // Create the viewport widget
    m_viewport = new ViewportWidget(this);
    m_viewport->GetViewportInteraction()->UpdateZoomFactorLabel();
    m_viewport->setFocusPolicy(Qt::StrongFocus);

    // Add the viewport widget to the layout of the central widget 
    centralWidgetLayout->addWidget(m_viewport);

    setCentralWidget(centralWidget);

    // Signal: Hierarchical tree -> Properties pane.
    QObject::connect(m_hierarchy,
        SIGNAL(SetUserSelection(HierarchyItemRawPtrList*)),
        m_properties->GetProperties(),
        SLOT(UserSelectionChanged(HierarchyItemRawPtrList*)));

    // Signal: Hierarchical tree -> Viewport pane.
    QObject::connect(m_hierarchy,
        SIGNAL(SetUserSelection(HierarchyItemRawPtrList*)),
        GetViewport(),
        SLOT(UserSelectionChanged(HierarchyItemRawPtrList*)));


    QObject::connect(m_undoGroup, &QUndoGroup::cleanChanged, this, &EditorWindow::CleanChanged);

    // by default the BottomDockWidgetArea will be the full width of the main window
    // and will make the Hierarchy and Properties panes less tall. This makes the
    // Hierarchy and Properties panes occupy the corners and makes the animation pane less wide.
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    // Hierarchy pane.
    {
        m_hierarchyDockWidget = new AzQtComponents::StyledDockWidget("Hierarchy");
        m_hierarchyDockWidget->setObjectName("HierarchyDockWidget");    // needed to save state
        m_hierarchyDockWidget->setWidget(m_hierarchy);
        // needed to get keyboard shortcuts properly
        m_hierarchy->setFocusPolicy(Qt::StrongFocus);
        addDockWidget(Qt::LeftDockWidgetArea, m_hierarchyDockWidget, Qt::Vertical);
    }

    // Properties pane.
    {
        m_propertiesDockWidget = new AzQtComponents::StyledDockWidget("Properties");
        m_propertiesDockWidget->setObjectName("PropertiesDockWidget");    // needed to save state
        m_propertiesDockWidget->setWidget(m_properties);
        m_properties->setFocusPolicy(Qt::StrongFocus);
        addDockWidget(Qt::RightDockWidgetArea, m_propertiesDockWidget, Qt::Vertical);
    }

    // Animation pane.
    {
        m_animationDockWidget = new AzQtComponents::StyledDockWidget("Animation Editor");
        m_animationDockWidget->setObjectName("AnimationDockWidget");    // needed to save state
        m_animationDockWidget->setWidget(m_animationWidget);
        m_animationWidget->setFocusPolicy(Qt::StrongFocus);
        addDockWidget(Qt::BottomDockWidgetArea, m_animationDockWidget, Qt::Horizontal);
    }

    // Preview action log pane (only shown in preview mode)
    {
        m_previewActionLogDockWidget = new AzQtComponents::StyledDockWidget("Action Log");
        m_previewActionLogDockWidget->setObjectName("PreviewActionLog");    // needed to save state
        m_previewActionLogDockWidget->setWidget(m_previewActionLog);
        m_previewActionLog->setFocusPolicy(Qt::StrongFocus);
        addDockWidget(Qt::BottomDockWidgetArea, m_previewActionLogDockWidget, Qt::Horizontal);
    }

    // Preview animation list pane (only shown in preview mode)
    {
        m_previewAnimationListDockWidget = new AzQtComponents::StyledDockWidget("Animation List");
        m_previewAnimationListDockWidget->setObjectName("PreviewAnimationList");    // needed to save state
        m_previewAnimationListDockWidget->setWidget(m_previewAnimationList);
        m_previewAnimationList->setFocusPolicy(Qt::StrongFocus);
        addDockWidget(Qt::LeftDockWidgetArea, m_previewAnimationListDockWidget, Qt::Vertical);
    }

    // We start out in edit mode so hide the preview mode widgets
    m_previewActionLogDockWidget->hide();
    m_previewAnimationListDockWidget->hide();
    m_previewToolbar->hide();

    // Initialize the menus
    RefreshEditorMenu();

    GetIEditor()->RegisterNotifyListener(this);

    // Initialize the toolbars
    m_viewport->GetViewportInteraction()->InitializeToolbars();

    // Start listening for any queries on the UiEditorDLLBus
    UiEditorDLLBus::Handler::BusConnect();

    // Start listening for any queries on the UiEditorChangeNotificationBus
    UiEditorChangeNotificationBus::Handler::BusConnect();

    AzToolsFramework::AssetBrowser::AssetBrowserModelNotificationBus::Handler::BusConnect();

    FontNotificationBus::Handler::BusConnect();

    // Don't draw the viewport until the window is shown
    m_viewport->SetRedrawEnabled(false);

    // Create an empty canvas
    LoadCanvas("", true);

    QTimer::singleShot(0, this, SLOT(RestoreEditorWindowSettings()));
}

EditorWindow::~EditorWindow()
{
    AzToolsFramework::AssetBrowser::AssetBrowserModelNotificationBus::Handler::BusDisconnect();

    FontNotificationBus::Handler::BusDisconnect();

    QObject::disconnect(m_clipboardConnection);

    GetIEditor()->UnregisterNotifyListener(this);

    UiEditorDLLBus::Handler::BusDisconnect();
    UiEditorChangeNotificationBus::Handler::BusDisconnect();

    // This has to be disconnected, or we'll get some weird feedback loop
    // where the cleanChanged signal propagates back up to the EditorWindow's
    // tab control, which is possibly already deleted, and everything explodes
    QObject::disconnect(m_undoGroup, &QUndoGroup::cleanChanged, this, &EditorWindow::CleanChanged);

    // Destroy all loaded canvases
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        DestroyCanvas(*canvasMetadata);
        delete canvasMetadata;
    }

    m_activeCanvasEntityId.SetInvalid();
    // Tell the UI animation system that the active canvas has changed
    EBUS_EVENT(UiEditorAnimationBus, ActiveCanvasChanged);

    // unload the preview mode canvas if it exists (e.g. if we close the editor window while in preview mode)
    if (m_previewModeCanvasEntityId.IsValid())
    {
        gEnv->pLyShine->ReleaseCanvas(m_previewModeCanvasEntityId, false);
    }

    delete m_sliceLibraryTree;

    delete m_sliceManager;

    // We must restore the original loc folder CVar value otherwise we will
    // have no way of obtaining the original loc folder location (in case
    // the user chooses to open the UI Editor once more).
    RestoreStartupLocalizationFolderSetting();
}

LyShine::EntityArray EditorWindow::GetSelectedElements()
{
    LyShine::EntityArray elements = SelectionHelpers::GetSelectedElements(
        m_hierarchy,
        m_hierarchy->selectedItems());

    return elements;
}

AZ::EntityId EditorWindow::GetActiveCanvasId()
{
    return GetCanvas();
}

UndoStack* EditorWindow::GetActiveUndoStack()
{
    return GetActiveStack();
}

void EditorWindow::OnEditorTransformPropertiesNeedRefresh()
{
    AZ::Uuid transformComponentUuid = LyShine::UiTransform2dComponentUuid;
    GetProperties()->TriggerRefresh(AzToolsFramework::PropertyModificationRefreshLevel::Refresh_AttributesAndValues, &transformComponentUuid);
}

void EditorWindow::OnEditorPropertiesRefreshEntireTree()
{
    GetProperties()->TriggerRefresh(AzToolsFramework::PropertyModificationRefreshLevel::Refresh_EntireTree);
}

void EditorWindow::OpenSourceCanvasFile(QString absolutePathToFile)
{
    // If in Preview mode, exit back to Edit mode
    if (m_editorMode == UiEditorMode::Preview)
    {
        ToggleEditorMode();
    }

    OpenCanvas(absolutePathToFile);
}

void EditorWindow::EntryAdded(const AzToolsFramework::AssetBrowser::AssetBrowserEntry* /*entry*/)
{
    DeleteSliceLibraryTree();
}

void EditorWindow::EntryRemoved(const AzToolsFramework::AssetBrowser::AssetBrowserEntry* /*entry*/)
{
    DeleteSliceLibraryTree();
}

void EditorWindow::OnFontsReloaded()
{
    OnEditorPropertiesRefreshEntireTree();
}

void EditorWindow::DestroyCanvas(const UiCanvasMetadata& canvasMetadata)
{
    // Submit metrics for a canvas that has changed since it was last
    // loaded/created, and all changes have been saved.
    if (canvasMetadata.m_canvasChangedAndSaved && !GetChangesHaveBeenMade(canvasMetadata))
    {
        SubmitUnloadSavedCanvasMetricEvent(canvasMetadata.m_canvasEntityId);
    }

    gEnv->pLyShine->ReleaseCanvas(canvasMetadata.m_canvasEntityId, true);
}

bool EditorWindow::IsCanvasTabMetadataValidForTabIndex(int index)
{
    QVariant data = m_canvasTabBar->tabData(index);
    return data.isValid();
}

AZ::EntityId EditorWindow::GetCanvasEntityIdForTabIndex(int index)
{
    QVariant data = m_canvasTabBar->tabData(index);
    AZ_Assert(data.isValid(), "Canvas tab metadata is not valid");
    if (data.isValid())
    {
        auto canvasTabMetadata = data.value<UiCanvasTabMetadata>();
        return canvasTabMetadata.m_canvasEntityId;
    }

    return AZ::EntityId();
}

int EditorWindow::GetTabIndexForCanvasEntityId(AZ::EntityId canvasEntityId)
{
    for (int i = 0; i < m_canvasTabBar->count(); i++)
    {
        if (GetCanvasEntityIdForTabIndex(i) == canvasEntityId)
        {
            return i;
        }
    }

    return -1;
}

EditorWindow::UiCanvasMetadata* EditorWindow::GetCanvasMetadataForTabIndex(int index)
{
    return GetCanvasMetadata(GetCanvasEntityIdForTabIndex(index));
}

EditorWindow::UiCanvasMetadata* EditorWindow::GetCanvasMetadata(AZ::EntityId canvasEntityId)
{
    auto canvasMetadataMapIt = m_canvasMetadataMap.find(canvasEntityId);
    return (canvasMetadataMapIt != m_canvasMetadataMap.end() ? canvasMetadataMapIt->second : nullptr);
}

EditorWindow::UiCanvasMetadata* EditorWindow::GetActiveCanvasMetadata()
{
    return GetCanvasMetadata(m_activeCanvasEntityId);
}

AZStd::string EditorWindow::GetCanvasDisplayNameFromAssetPath(const AZStd::string& canvasAssetPathname)
{
    QFileInfo fileInfo(canvasAssetPathname.c_str());
    QString canvasDisplayName(fileInfo.baseName());
    if (canvasDisplayName.isEmpty())
    {
        canvasDisplayName = QString("Canvas%1").arg(m_newCanvasCount++);
    }

    return AZStd::string(canvasDisplayName.toLatin1().data());
}

void EditorWindow::HandleCanvasDisplayNameChanged(const UiCanvasMetadata& canvasMetadata)
{
    // Update the tab label for the canvas
    AZStd::string tabText(canvasMetadata.m_canvasDisplayName);
    if (!canvasMetadata.m_undoStack->isClean())
    {
        tabText.append("*");
    }
    int tabIndex = GetTabIndexForCanvasEntityId(canvasMetadata.m_canvasEntityId);
    m_canvasTabBar->setTabText(tabIndex, tabText.c_str());
    m_canvasTabBar->setTabToolTip(tabIndex, canvasMetadata.m_canvasSourceAssetPathname.empty() ? canvasMetadata.m_canvasDisplayName.c_str() : canvasMetadata.m_canvasSourceAssetPathname.c_str());
}

void EditorWindow::CleanChanged(bool clean)
{
    UiCanvasMetadata *canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        HandleCanvasDisplayNameChanged(*canvasMetadata);
    }
}

bool EditorWindow::SaveCanvasToXml(UiCanvasMetadata& canvasMetadata, bool forceAskingForFilename)
{
    AZStd::string sourceAssetPathName = canvasMetadata.m_canvasSourceAssetPathname;
    AZStd::string assetIdPathname;

    if (!forceAskingForFilename)
    {
        // Before saving, make sure the file contains an extension we're expecting
        QString filename = sourceAssetPathName.c_str();
        if ((!filename.isEmpty()) &&
            (!FileHelpers::FilenameHasExtension(filename, UICANVASEDITOR_CANVAS_EXTENSION)))
        {
            QMessageBox::warning(this, tr("Warning"), tr("Please save with the expected extension: *.%1").arg(UICANVASEDITOR_CANVAS_EXTENSION));
            forceAskingForFilename = true;
        }
    }

    if (sourceAssetPathName.empty() || forceAskingForFilename)
    {
        // Default the pathname to where the current canvas was loaded from or last saved to

        QString dir;
        QStringList recentFiles = ReadRecentFiles();

        // If the canvas we are saving already has a name
        if (!sourceAssetPathName.empty())
        {
            // Default to where it was loaded from or last saved to
            // Also notice that we directly assign dir to the filename
            // This allows us to have its existing name already entered in
            // the File Name field.
            dir = sourceAssetPathName.c_str();
        }
        // Else if we had recently opened canvases, open the most recent one's directory
        else if (recentFiles.size() > 0)
        {
            dir = Path::GetPath(recentFiles.front());
            dir.append(canvasMetadata.m_canvasDisplayName.c_str());
        }
        // Else go to the default canvas directory
        else
        {
            dir = FileHelpers::GetAbsoluteDir(UICANVASEDITOR_CANVAS_DIRECTORY);
            dir.append(canvasMetadata.m_canvasDisplayName.c_str());
        }

        QString filename = QFileDialog::getSaveFileName(nullptr,
            QString(),
            dir,
            "*." UICANVASEDITOR_CANVAS_EXTENSION,
            nullptr,
            QFileDialog::DontConfirmOverwrite);
        if (filename.isEmpty())
        {
            return false;
        }

        // Append extension if not present
        FileHelpers::AppendExtensionIfNotPresent(filename, UICANVASEDITOR_CANVAS_EXTENSION);

        sourceAssetPathName = filename.toUtf8().data();

        // Check if the canvas is being saved in the product path
        bool foundRelativePath = false;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(foundRelativePath, &AzToolsFramework::AssetSystem::AssetSystemRequest::GetRelativeProductPathFromFullSourceOrProductPath, sourceAssetPathName, assetIdPathname);
        if (!foundRelativePath)
        {
            // Warn that canvas is being saved outside the product path
            int result = QMessageBox::warning(this,
                tr("Warning"),
                tr("UI canvas %1 is being saved outside the source folder for the project (or the Asset Processor is not running).\n\nSaving to this location will result in not being able to re-open the UI Canvas in the UI Editor from this location.\n\nWould you still like to save to this location?").arg(filename),
                (QMessageBox::Save | QMessageBox::Cancel),
                QMessageBox::Cancel);

            if (result == QMessageBox::Save)
            {
                assetIdPathname = Path::FullPathToGamePath(sourceAssetPathName.c_str()); // Relative path.
            }
            else
            {
                return false;
            }
        }
    }
    else
    {
        sourceAssetPathName = canvasMetadata.m_canvasSourceAssetPathname;
        EBUS_EVENT_ID_RESULT(assetIdPathname, canvasMetadata.m_canvasEntityId, UiCanvasBus, GetPathname);
    }

    FileHelpers::SourceControlAddOrEdit(sourceAssetPathName.c_str(), this);

    bool saveSuccessful = false;
    EBUS_EVENT_ID_RESULT(saveSuccessful, canvasMetadata.m_canvasEntityId, UiCanvasBus, SaveToXml,
        assetIdPathname.c_str(), sourceAssetPathName.c_str());

    if (saveSuccessful)
    {
        AddRecentFile(sourceAssetPathName.c_str());

        if (!canvasMetadata.m_canvasChangedAndSaved)
        {
            canvasMetadata.m_canvasChangedAndSaved = GetChangesHaveBeenMade(canvasMetadata);
        }
        canvasMetadata.m_canvasSourceAssetPathname = sourceAssetPathName;

        AZStd::string newDisplayName = GetCanvasDisplayNameFromAssetPath(canvasMetadata.m_canvasSourceAssetPathname);
        if (canvasMetadata.m_canvasDisplayName != newDisplayName)
        {
            canvasMetadata.m_canvasDisplayName = newDisplayName;
        }

        canvasMetadata.m_undoStack->setClean();

        HandleCanvasDisplayNameChanged(canvasMetadata);

        return true;
    }

    QMessageBox(QMessageBox::Critical,
        "Error",
        tr("Unable to save %1. Is the file read-only?").arg(sourceAssetPathName.empty() ? "file" : sourceAssetPathName.c_str()),
        QMessageBox::Ok, this).exec();

    return false;
}

void EditorWindow::LoadCanvas(const QString& canvasFilename, bool autoLoad, bool changeActiveCanvasToThis)
{
    // Don't allow a new canvas to load if there is a context menu up since loading doesn't
    // delete the context menu. Another option is to close the context menu on canvas load,
    // but the main editor's behavior seems to be to ignore the main keyboard shortcuts if
    // a context menu is up
    QWidget* widget = QApplication::activePopupWidget();
    if (widget)
    {
        return;
    }

    AZStd::string assetIdPathname;
    AZStd::string sourceAssetPathName;
    if (!canvasFilename.isEmpty())
    {
        // Get the relative product path of the canvas to load
        bool foundRelativePath = false;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(foundRelativePath, &AzToolsFramework::AssetSystem::AssetSystemRequest::GetRelativeProductPathFromFullSourceOrProductPath, canvasFilename.toUtf8().data(), assetIdPathname);
        if (!foundRelativePath)
        {
            // Canvas to load is not in a project source folder. Report an error
            QMessageBox::critical(this, tr("Error"), tr("Failed to open %1. Please ensure the file resides in a valid source folder for the project and that the Asset Processor is running.").arg(canvasFilename));
            return;
        }

        // Get the path to the source UI Canvas from the relative product path
        // This is done because a canvas could be loaded from the cache folder. In this case, we want to find the path to the source file
        bool fullPathfound = false;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(fullPathfound, &AzToolsFramework::AssetSystemRequestBus::Events::GetFullSourcePathFromRelativeProductPath, assetIdPathname, sourceAssetPathName);
        if (!fullPathfound)
        {
            // Couldn't find the source file. Report an error
            QMessageBox::critical(this, tr("Error"), tr("Failed to find the source file for UI canvas %1. Please ensure that the Asset Processor is running and that the source file exists").arg(canvasFilename));
            return;
        }
    }

    // Check if canvas is already loaded
    AZ::EntityId alreadyLoadedCanvas;
    if (!canvasFilename.isEmpty())
    {
        for (auto mapItem : m_canvasMetadataMap)
        {
            auto canvasMetadata = mapItem.second;
            if (canvasMetadata->m_canvasSourceAssetPathname == sourceAssetPathName)
            {
                alreadyLoadedCanvas = canvasMetadata->m_canvasEntityId;
                break;
            }
        }
    }

    if (alreadyLoadedCanvas.IsValid())
    {
        // Canvas is already loaded
        if (changeActiveCanvasToThis)
        {
            if (CanChangeActiveCanvas())
            {
                SetActiveCanvas(alreadyLoadedCanvas);
            }
        }
        return;
    }

    AZ::EntityId canvasEntityId;
    UiEditorEntityContext* entityContext = new UiEditorEntityContext(this);

    // Load the canvas
    if (canvasFilename.isEmpty())
    {
        canvasEntityId = gEnv->pLyShine->CreateCanvasInEditor(entityContext);
    }
    else
    {
        canvasEntityId = gEnv->pLyShine->LoadCanvasInEditor(assetIdPathname.c_str(), sourceAssetPathName.c_str(), entityContext);
        if (canvasEntityId.IsValid())
        {
            AddRecentFile(sourceAssetPathName.c_str());
        }
        else
        {
            // There was an error loading the file. Report an error
            QMessageBox::critical(this, tr("Error"), tr("Failed to load UI canvas %1. See log for details").arg(sourceAssetPathName.c_str()));
        }
    }

    if (!canvasEntityId.IsValid())
    {
        delete entityContext;
        return;
    }

    // Add a canvas tab
    AZStd::string canvasDisplayName = GetCanvasDisplayNameFromAssetPath(sourceAssetPathName);

    int newTabIndex = m_canvasTabBar->addTab(canvasDisplayName.c_str()); // this will call OnCurrentCanvasTabChanged if first tab, but nothing will happen because the metadata won't be set yet
    UiCanvasTabMetadata tabMetadata;
    tabMetadata.m_canvasEntityId = canvasEntityId;
    m_canvasTabBar->setTabData(newTabIndex, QVariant::fromValue(tabMetadata));
    m_canvasTabBar->setTabToolTip(newTabIndex, sourceAssetPathName.empty() ? canvasDisplayName.c_str() : sourceAssetPathName.c_str());

    UiCanvasMetadata* canvasMetadata = new UiCanvasMetadata;
    canvasMetadata->m_canvasEntityId = canvasEntityId;
    canvasMetadata->m_canvasSourceAssetPathname = sourceAssetPathName;
    canvasMetadata->m_canvasDisplayName = canvasDisplayName;
    canvasMetadata->m_entityContext = entityContext;
    canvasMetadata->m_undoStack = new UndoStack(m_undoGroup);
    canvasMetadata->m_autoLoaded = autoLoad;
    canvasMetadata->m_canvasChangedAndSaved = false;

    // Check if there is an automatically created canvas that should be unloaded.
    // Unload an automatically created canvas if:
    // 1. it's the only loaded canvas
    // 2. changes have not been made to it
    // 3. the newly loaded canvas is not a new canvas
    AZ::EntityId unloadCanvasEntityId;
    if (!canvasMetadata->m_canvasSourceAssetPathname.empty())
    {
        if (m_canvasMetadataMap.size() == 1)
        {
            UiCanvasMetadata *unloadCanvasMetadata = GetActiveCanvasMetadata();
            if (unloadCanvasMetadata && unloadCanvasMetadata->m_autoLoaded)
            {
                // Check if there are changes to this canvas
                if (unloadCanvasMetadata->m_canvasSourceAssetPathname.empty() && !GetChangesHaveBeenMade(*unloadCanvasMetadata))
                {
                    unloadCanvasEntityId = unloadCanvasMetadata->m_canvasEntityId;
                }
            }
        }
    }

    // Add the newly loaded canvas to the map
    m_canvasMetadataMap[canvasEntityId] = canvasMetadata;

    // Make the newly loaded canvas the active canvas
    if (changeActiveCanvasToThis || !m_activeCanvasEntityId.IsValid())
    {
        if (CanChangeActiveCanvas())
        {
            SetActiveCanvas(canvasEntityId);
        }
    }

    // If there was an automatically created empty canvas, unload it
    if (unloadCanvasEntityId.IsValid())
    {
        UnloadCanvas(unloadCanvasEntityId);
    }
}

void EditorWindow::UnloadCanvas(AZ::EntityId canvasEntityId)
{
    UiCanvasMetadata* canvasMetadata = GetCanvasMetadata(canvasEntityId);
    if (canvasMetadata)
    {
        // Delete the canvas
        DestroyCanvas(*canvasMetadata);

        // Remove the undo stack from the undo group
        m_undoGroup->removeStack(canvasMetadata->m_undoStack);

        // Remove the canvas metadata from the list of loaded canvases
        m_canvasMetadataMap.erase(canvasMetadata->m_canvasEntityId);
        delete canvasMetadata;

        // Remove the tab associated with this canvas
        // OnCurrentCanvasTabChanged will be called, and the active canvas will be updated
        int tabIndex = GetTabIndexForCanvasEntityId(canvasEntityId);
        m_canvasTabBar->removeTab(tabIndex);

        // Ensure the active canvas is valid in case removeTab didn't cause it to change or the implementation changed
        if (!GetCanvasMetadata(m_activeCanvasEntityId))
        {
            if (IsCanvasTabMetadataValidForTabIndex(m_canvasTabBar->currentIndex()))
            {
                SetActiveCanvas(GetCanvasEntityIdForTabIndex(m_canvasTabBar->currentIndex()));
            }
            else
            {
                SetActiveCanvas(AZ::EntityId());
            }
        }
    }
}

void EditorWindow::NewCanvas()
{
    LoadCanvas("", false);
}

void EditorWindow::OpenCanvas(const QString& canvasFilename)
{
    LoadCanvas(canvasFilename, false);
}

void EditorWindow::OpenCanvases(const QStringList& canvasFilenames)
{
    for (int i = 0; i < canvasFilenames.size(); i++)
    {
        LoadCanvas(canvasFilenames.at(i), false, (i == 0));
    }
}

void EditorWindow::CloseCanvas(AZ::EntityId canvasEntityId)
{
    UiCanvasMetadata* canvasMetadata = GetCanvasMetadata(canvasEntityId);
    if (canvasMetadata)
    {
        if (CanUnloadCanvas(*canvasMetadata))
        {
            UnloadCanvas(canvasMetadata->m_canvasEntityId);
        }
    }
}

void EditorWindow::CloseAllCanvases()
{
    if (!m_activeCanvasEntityId.IsValid())
    {
        return;
    }

    // Check if all canvases can be unloaded
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (!CanUnloadCanvas(*canvasMetadata))
        {
            return;
        }
    }

    // Make a list of canvases to unload. Unload the active canvas last so that the
    // active canvas doesn't keep changing when the canvases are unloaded one by one
    AZStd::vector<AZ::EntityId> canvasEntityIds;
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (canvasMetadata->m_canvasEntityId != m_activeCanvasEntityId)
        {
            canvasEntityIds.push_back(canvasMetadata->m_canvasEntityId);
        }
    }
    canvasEntityIds.push_back(m_activeCanvasEntityId);

    UnloadCanvases(canvasEntityIds);
}

void EditorWindow::CloseAllOtherCanvases(AZ::EntityId canvasEntityId)
{
    if (m_canvasMetadataMap.size() < 2)
    {
        return;
    }

    // Check if all but the specified canvas can be unloaded
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (canvasMetadata->m_canvasEntityId != canvasEntityId)
        {
            if (!CanUnloadCanvas(*canvasMetadata))
            {
                return;
            }
        }
    }

    // Make a list of canvases to unload
    AZStd::vector<AZ::EntityId> canvasEntityIds;
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (canvasMetadata->m_canvasEntityId != canvasEntityId)
        {
            canvasEntityIds.push_back(canvasMetadata->m_canvasEntityId);
        }
    }

    UnloadCanvases(canvasEntityIds);

    // Update the menus for file/save/close
    RefreshEditorMenu();
}

bool EditorWindow::CanChangeActiveCanvas()
{
    UiCanvasMetadata *canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        if (canvasMetadata->m_entityContext->HasPendingRequests() || canvasMetadata->m_entityContext->IsInstantiatingSlices())
        {
            return false;
        }
    }

    return true;
}

void EditorWindow::SetActiveCanvas(AZ::EntityId canvasEntityId)
{
    // This function is called explicitly to set the current active canvas (when a new canvas is loaded).
    // This function is also called from the OnCurrentCanvasTabChanged event handler that is triggered by a user action
    // that changes the tab index (closing a tab or clicking on a different tab)

    if (canvasEntityId == m_activeCanvasEntityId)
    {
        return;
    }

    // Don't redraw the viewport until the active tab has visually changed
    m_viewport->SetRedrawEnabled(false);

    // Disable previous active canvas
    if (m_activeCanvasEntityId.IsValid())
    {
        // Disable undo stack
        UiCanvasMetadata* canvasMetadata = GetActiveCanvasMetadata();
        if (canvasMetadata)
        {
            canvasMetadata->m_undoStack->setActive(false);
        }

        // Save canvas edit state
        SaveActiveCanvasEditState();
    }

    // Update the active canvas Id
    m_activeCanvasEntityId = canvasEntityId;

    // Set the current tab index to that of the active canvas.
    // If this function was called explicitly (when a new canvas is loaded), setCurrentIndex will trigger OnCurrentCanvasTabChanged, and OnCurrentCanvasTabChanged
    // will call us back, but we will early out because the new active canvas will be the same as the current active canvas.
    // If this function was called from the OnCurrentCanvasTabChanged event handler (triggered by a user clicking on a tab or a user closing a tab),
    // the new tab index will be the same as the current tab index so no more events will be triggered by calling setCurrentIndex here
    m_canvasTabBar->setCurrentIndex(GetTabIndexForCanvasEntityId(m_activeCanvasEntityId));

    // Get the new active canvas's metadata
    UiCanvasMetadata* canvasMetadata = m_activeCanvasEntityId.IsValid() ? GetCanvasMetadata(m_activeCanvasEntityId) : nullptr;

    // Enable new active canvas
    if (canvasMetadata)
    {
        canvasMetadata->m_undoStack->setActive(true);
    }

    // Update the slice manager 
    m_sliceManager->SetEntityContextId(canvasMetadata ? canvasMetadata->m_entityContext->GetContextId() : AzFramework::EntityContextId::CreateNull());

    // Tell the UI animation system that the active canvas has changed
    EBUS_EVENT(UiEditorAnimationBus, ActiveCanvasChanged);

    // Clear the hierarchy pane
    m_hierarchy->ClearItems();

    if (m_activeCanvasEntityId.IsValid())
    {
        // create the hierarchy tree from the loaded canvas
        LyShine::EntityArray childElements;
        EBUS_EVENT_ID_RESULT(childElements, m_activeCanvasEntityId, UiCanvasBus, GetChildElements);
        m_hierarchy->CreateItems(childElements);

        // restore the expanded state of all items
        m_hierarchy->ApplyElementIsExpanded();
    }

    m_hierarchy->clearSelection();
    m_hierarchy->SetUserSelection(nullptr); // trigger a selection change so the properties updates

    m_viewport->ActiveCanvasChanged();

    RefreshEditorMenu();

    // Restore Canvas edit state
    RestoreActiveCanvasEditState();

    m_properties->ActiveCanvasChanged();

    // Do the rest of the restore after all other events have had a chance to process because
    // The hierarchy and properties scrollbars have not been set up yet
    QTimer::singleShot(0, this, &EditorWindow::RestoreActiveCanvasEditStatePostEvents);
}

void EditorWindow::SaveActiveCanvasEditState()
{
    UiCanvasMetadata* canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        UiCanvasEditState& canvasEditState = canvasMetadata->m_canvasEditState;

        // Save viewport state
        canvasEditState.m_canvasViewportMatrixProps = m_viewport->GetViewportInteraction()->GetCanvasViewportMatrixProps();
        canvasEditState.m_shouldScaleToFitOnViewportResize = m_viewport->GetViewportInteraction()->ShouldScaleToFitOnViewportResize();
        canvasEditState.m_viewportInteractionMode = m_viewport->GetViewportInteraction()->GetMode();
        canvasEditState.m_viewportCoordinateSystem = m_viewport->GetViewportInteraction()->GetCoordinateSystem();

        // Save hierarchy state
        const QTreeWidgetItemRawPtrQList& selection = m_hierarchy->selectedItems();
        canvasEditState.m_selectedElements = SelectionHelpers::GetSelectedElementIds(m_hierarchy, selection, false);
        canvasEditState.m_hierarchyScrollValue = m_hierarchy->verticalScrollBar() ? m_hierarchy->verticalScrollBar()->value() : 0.0f;

        // Save properties state
        canvasEditState.m_propertiesScrollValue = m_properties->GetProperties()->GetScrollValue();

        // Save animation state
        canvasEditState.m_uiAnimationEditState.m_time = 0.0f;
        canvasEditState.m_uiAnimationEditState.m_timelineScale = 1.0f;
        canvasEditState.m_uiAnimationEditState.m_timelineScrollOffset = 0.0f;
        EBUS_EVENT_RESULT(canvasEditState.m_uiAnimationEditState, UiEditorAnimationStateBus, GetCurrentEditState);

        canvasEditState.m_inited = true;
    }
}

void EditorWindow::RestoreActiveCanvasEditState()
{
    UiCanvasMetadata* canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        const UiCanvasEditState& canvasEditState = canvasMetadata->m_canvasEditState;
        if (canvasEditState.m_inited)
        {
            // Restore viewport state
            m_viewport->GetViewportInteraction()->SetCanvasViewportMatrixProps(canvasEditState.m_canvasViewportMatrixProps);
            if (canvasEditState.m_shouldScaleToFitOnViewportResize)
            {
                m_viewport->GetViewportInteraction()->CenterCanvasInViewport();
            }
            m_viewport->GetViewportInteraction()->SetCoordinateSystem(canvasEditState.m_viewportCoordinateSystem);
            m_viewport->GetViewportInteraction()->SetMode(canvasEditState.m_viewportInteractionMode);

            // Restore hierarchy state
            HierarchyHelpers::SetSelectedItems(m_hierarchy, &canvasEditState.m_selectedElements);

            // Restore animation state
            EBUS_EVENT(UiEditorAnimationStateBus, RestoreCurrentEditState, canvasEditState.m_uiAnimationEditState);
        }
    }
}

void EditorWindow::RestoreActiveCanvasEditStatePostEvents()
{
    UiCanvasMetadata* canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        const UiCanvasEditState& canvasEditState = canvasMetadata->m_canvasEditState;
        if (canvasEditState.m_inited)
        {
            // Restore hierarchy state
            if (m_hierarchy->verticalScrollBar())
            {
                m_hierarchy->verticalScrollBar()->setValue(canvasEditState.m_hierarchyScrollValue);
            }

            // Restore properties state
            m_properties->GetProperties()->SetScrollValue(canvasEditState.m_propertiesScrollValue);
        }
    }

    m_viewport->SetRedrawEnabled(true);
    m_viewport->setFocus();
}

void EditorWindow::UnloadCanvases(const AZStd::vector<AZ::EntityId>& canvasEntityIds)
{
    for (int i = 0; i < canvasEntityIds.size(); i++)
    {
        UnloadCanvas(canvasEntityIds[i]);
    }
}

AZ::EntityId EditorWindow::GetCanvas()
{
    return m_activeCanvasEntityId;
}

HierarchyWidget* EditorWindow::GetHierarchy()
{
    AZ_Assert(m_hierarchy, "Missing hierarchy widget");
    return m_hierarchy;
}

ViewportWidget* EditorWindow::GetViewport()
{
    AZ_Assert(m_viewport, "Missing viewport widget");
    return m_viewport;
}

PropertiesWidget* EditorWindow::GetProperties()
{
    AZ_Assert(m_properties, "Missing properties wrapper");
    AZ_Assert(m_properties->GetProperties(), "Missing properties widget");
    return m_properties->GetProperties();
}

MainToolbar* EditorWindow::GetMainToolbar()
{
    AZ_Assert(m_mainToolbar, "Missing main toolbar");
    return m_mainToolbar;
}

ModeToolbar* EditorWindow::GetModeToolbar()
{
    AZ_Assert(m_modeToolbar, "Missing mode toolbar");
    return m_modeToolbar;
}

EnterPreviewToolbar* EditorWindow::GetEnterPreviewToolbar()
{
    AZ_Assert(m_enterPreviewToolbar, "Missing enter preview toolbar");
    return m_enterPreviewToolbar;
}

PreviewToolbar* EditorWindow::GetPreviewToolbar()
{
    AZ_Assert(m_previewToolbar, "Missing preview toolbar");
    return m_previewToolbar;
}

NewElementToolbarSection* EditorWindow::GetNewElementToolbarSection()
{
    AZ_Assert(m_mainToolbar, "Missing main toolbar");
    return m_mainToolbar->GetNewElementToolbarSection();
}

CoordinateSystemToolbarSection* EditorWindow::GetCoordinateSystemToolbarSection()
{
    AZ_Assert(m_mainToolbar, "Missing main toolbar");
    return m_mainToolbar->GetCoordinateSystemToolbarSection();
}

CanvasSizeToolbarSection* EditorWindow::GetCanvasSizeToolbarSection()
{
    AZ_Assert(m_mainToolbar, "Missing main toolbar");
    return m_mainToolbar->GetCanvasSizeToolbarSection();
}

void EditorWindow::OnEditorNotifyEvent(EEditorNotifyEvent ev)
{
    switch (ev)
    {
    case eNotify_OnIdleUpdate:
        m_viewport->Refresh();
        break;
    case eNotify_OnStyleChanged:
    {
        // change skin
        RefreshEditorMenu();
        m_viewport->UpdateViewportBackground();
        break;
    }
    case eNotify_OnUpdateViewports:
    {
        // provides a way for the animation editor to force updates of the properties dialog during
        // an animation
        GetProperties()->TriggerRefresh(AzToolsFramework::PropertyModificationRefreshLevel::Refresh_Values);
        break;
    }
    }
}

bool EditorWindow::CanExitNow()
{
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (!CanUnloadCanvas(*canvasMetadata))
        {
            return false;
        }
    }

    return true;
}

bool EditorWindow::CanUnloadCanvas(UiCanvasMetadata& canvasMetadata)
{
    if (GetChangesHaveBeenMade(canvasMetadata))
    {
        const auto defaultButton = QMessageBox::Cancel;
        int result = QMessageBox::question(this,
            tr("Changes have been made"),
            tr("Save changes to UI canvas %1?").arg(canvasMetadata.m_canvasDisplayName.c_str()),
            (QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel),
            defaultButton);

        if (result == QMessageBox::Save)
        {
            bool ok = SaveCanvasToXml(canvasMetadata, false);
            if (!ok)
            {
                return false;
            }
        }
        else if (result == QMessageBox::Discard)
        {
            // Nothing to do
        }
        else // if( result == QMessageBox::Cancel )
        {
            return false;
        }
    }

    return true;
}

bool EditorWindow::GetChangesHaveBeenMade(const UiCanvasMetadata& canvasMetadata)
{
    return !canvasMetadata.m_undoStack->isClean();
}

QUndoGroup* EditorWindow::GetUndoGroup()
{
    return m_undoGroup;
}

UndoStack* EditorWindow::GetActiveStack()
{
    return qobject_cast<UndoStack*>(m_undoGroup->activeStack());
}

AssetTreeEntry* EditorWindow::GetSliceLibraryTree()
{
    if (!m_sliceLibraryTree)
    {
        const AZStd::string pathToSearch("ui/slices/library/");
        const AZ::Data::AssetType sliceAssetType(AZ::AzTypeInfo<AZ::SliceAsset>::Uuid());

        m_sliceLibraryTree = AssetTreeEntry::BuildAssetTree(sliceAssetType, pathToSearch);
    }

    return m_sliceLibraryTree;
}

void EditorWindow::UpdatePrefabFiles()
{
    m_prefabFiles.clear();

    // IMPORTANT: ScanDirectory() is VERY slow. It can easily take as much
    // as a whole second to execute. That's why we want to cache its result
    // up front and ONLY access the cached data.
    GetIEditor()->GetFileUtil()->ScanDirectory("", "*." UICANVASEDITOR_PREFAB_EXTENSION, m_prefabFiles);
    SortPrefabsList();
}

IFileUtil::FileArray& EditorWindow::GetPrefabFiles()
{
    return m_prefabFiles;
}

void EditorWindow::AddPrefabFile(const QString& prefabFilename)
{
    IFileUtil::FileDesc fd;
    fd.filename = prefabFilename;
    m_prefabFiles.push_back(fd);
    SortPrefabsList();
}

void EditorWindow::SortPrefabsList()
{
    AZStd::sort<IFileUtil::FileArray::iterator>(m_prefabFiles.begin(), m_prefabFiles.end(),
        [](const IFileUtil::FileDesc& fd1, const IFileUtil::FileDesc& fd2)
    {
        // Some of the files in the list are in different directories, so we
        // explicitly sort by filename only.
        AZStd::string fd1Filename;
        AzFramework::StringFunc::Path::GetFileName(fd1.filename.toUtf8().data(), fd1Filename);

        AZStd::string fd2Filename;
        AzFramework::StringFunc::Path::GetFileName(fd2.filename.toUtf8().data(), fd2Filename);
        return fd1Filename < fd2Filename;
    });
}

void EditorWindow::ToggleEditorMode()
{
    m_editorMode = (m_editorMode == UiEditorMode::Edit) ? UiEditorMode::Preview : UiEditorMode::Edit;

    emit EditorModeChanged(m_editorMode);

    m_viewport->ClearUntilSafeToRedraw();

    if (m_editorMode == UiEditorMode::Edit)
    {
        // unload the preview mode canvas
        if (m_previewModeCanvasEntityId.IsValid())
        {
            m_previewActionLog->Deactivate();
            m_previewAnimationList->Deactivate();

            AZ::Entity* entity = nullptr;
            EBUS_EVENT_RESULT(entity, AZ::ComponentApplicationBus, FindEntity, m_previewModeCanvasEntityId);
            if (entity)
            {
                gEnv->pLyShine->ReleaseCanvas(m_previewModeCanvasEntityId, false);
            }
            m_previewModeCanvasEntityId.SetInvalid();
        }

        m_canvasTabSectionWidget->show();

        SaveModeSettings(UiEditorMode::Preview, false);
        RestoreModeSettings(UiEditorMode::Edit);
    }
    else
    {
        m_canvasTabSectionWidget->hide();

        SaveModeSettings(UiEditorMode::Edit, false);
        RestoreModeSettings(UiEditorMode::Preview);

        GetPreviewToolbar()->UpdatePreviewCanvasScale(m_viewport->GetPreviewCanvasScale());

        // clone the editor canvas to create a temporary preview mode canvas
        if (m_activeCanvasEntityId.IsValid())
        {
            AZ_Assert(!m_previewModeCanvasEntityId.IsValid(), "There is an existing preview mode canvas");

            // Get the canvas size
            AZ::Vector2 canvasSize = GetPreviewCanvasSize();
            if (canvasSize.GetX() == 0.0f && canvasSize.GetY() == 0.0f)
            {
                // special value of (0,0) means use the viewport size
                canvasSize = AZ::Vector2(m_viewport->size().width(), m_viewport->size().height());
            }

            AZ::Entity* clonedCanvas = nullptr;
            EBUS_EVENT_ID_RESULT(clonedCanvas, m_activeCanvasEntityId, UiCanvasBus, CloneCanvas, canvasSize);

            if (clonedCanvas)
            {
                m_previewModeCanvasEntityId = clonedCanvas->GetId();
            }
        }

        m_previewActionLog->Activate(m_previewModeCanvasEntityId);

        m_previewAnimationList->Activate(m_previewModeCanvasEntityId);

        // In Preview mode we want keyboard input to go to to the ViewportWidget so set the
        // it to be focused
        m_viewport->setFocus();
    }

    // Update the menus for this mode
    RefreshEditorMenu();
}

AZ::Vector2 EditorWindow::GetPreviewCanvasSize()
{
    return m_previewModeCanvasSize;
}

void EditorWindow::SetPreviewCanvasSize(AZ::Vector2 previewCanvasSize)
{
    m_previewModeCanvasSize = previewCanvasSize;
}

bool EditorWindow::IsPreviewModeToolbar(const QToolBar* toolBar)
{
    bool result = false;
    if (toolBar == m_previewToolbar)
    {
        result = true;
    }
    return result;
}

bool EditorWindow::IsPreviewModeDockWidget(const QDockWidget* dockWidget)
{
    bool result = false;
    if (dockWidget == m_previewActionLogDockWidget ||
        dockWidget == m_previewAnimationListDockWidget)
    {
        result = true;
    }
    return result;
}

void EditorWindow::RestoreEditorWindowSettings()
{
    // Allow the editor window to draw now that we are ready to restore state.
    // Do this before restoring state, otherwise an undocked widget will not be affected by the call
    setUpdatesEnabled(true);

    RestoreModeSettings(m_editorMode);

    m_viewport->SetRedrawEnabled(true);
}

void EditorWindow::SaveEditorWindowSettings()
{
    // This saves the dock position, size and visibility of all the dock widgets and tool bars
    // for the current mode (it also syncs the settings for the other mode that have already been saved to settings)
    SaveModeSettings(m_editorMode, true);
}

UiSliceManager* EditorWindow::GetSliceManager()
{
    return m_sliceManager;
}

UiEditorEntityContext* EditorWindow::GetEntityContext()
{
    if (GetCanvas().IsValid())
    {
        auto canvasMetadata = GetActiveCanvasMetadata();
        AZ_Assert(canvasMetadata, "Canvas metadata not found");
        return canvasMetadata ? canvasMetadata->m_entityContext : nullptr;
    }

    return nullptr;
}

void EditorWindow::ReplaceEntityContext(UiEditorEntityContext* entityContext)
{
    UiCanvasMetadata* canvasMetadata = GetActiveCanvasMetadata();
    if (canvasMetadata)
    {
        delete canvasMetadata->m_entityContext;
        canvasMetadata->m_entityContext = entityContext;

        m_sliceManager->SetEntityContextId(entityContext->GetContextId());
    }
}

QMenu* EditorWindow::createPopupMenu()
{
    QMenu* menu = new QMenu(this);

    // Add all QDockWidget panes for the current editor mode
    {
        QList<QDockWidget*> list = findChildren<QDockWidget*>();

        for (auto p : list)
        {
            // findChildren is recursive, but we only want dock widgets that are immediate children
            if (p->parent() == this)
            {
                bool isPreviewModeDockWidget = IsPreviewModeDockWidget(p);
                if (m_editorMode == UiEditorMode::Edit && !isPreviewModeDockWidget ||
                    m_editorMode == UiEditorMode::Preview && isPreviewModeDockWidget)
                {
                    menu->addAction(p->toggleViewAction());
                }
            }
        }
    }

    // Add all QToolBar panes for the current editor mode
    {
        QList<QToolBar*> list = findChildren<QToolBar*>();
        for (auto p : list)
        {
            if (p->parent() == this)
            {
                bool isPreviewModeToolbar = IsPreviewModeToolbar(p);
                if (m_editorMode == UiEditorMode::Edit && !isPreviewModeToolbar ||
                    m_editorMode == UiEditorMode::Preview && isPreviewModeToolbar)
                {
                    menu->addAction(p->toggleViewAction());
                }
            }
        }
    }

    return menu;
}

AZ::EntityId EditorWindow::GetCanvasForEntityContext(const AzFramework::EntityContextId& contextId)
{
    for (auto mapItem : m_canvasMetadataMap)
    {
        auto canvasMetadata = mapItem.second;
        if (canvasMetadata->m_entityContext->GetContextId() == contextId)
        {
            return canvasMetadata->m_canvasEntityId;
        }
    }

    return AZ::EntityId();
}

void EditorWindow::OnCanvasTabCloseButtonPressed(int index)
{
    UiCanvasMetadata* canvasMetadata = GetCanvasMetadataForTabIndex(index);
    if (canvasMetadata)
    {
        if (CanUnloadCanvas(*canvasMetadata))
        {
            bool isActiveCanvas = (canvasMetadata->m_canvasEntityId == m_activeCanvasEntityId);
            UnloadCanvas(canvasMetadata->m_canvasEntityId);

            if (!isActiveCanvas)
            {
                // Update the menus for file/save/close
                RefreshEditorMenu();
            }
        }
    }
}

void EditorWindow::OnCurrentCanvasTabChanged(int index)
{
    // This is called when the first tab is added, when a tab is removed, or when a user clicks on a tab that's not the current tab

    // Get the canvas associated with this index
    AZ::EntityId canvasEntityId = IsCanvasTabMetadataValidForTabIndex(index) ? GetCanvasEntityIdForTabIndex(index) : AZ::EntityId();

    if (index >= 0 && !canvasEntityId.IsValid())
    {
        // This occurs when the first tab is added. Since the tab metadata is set after the tab is added, we don't handle this here.
        // Instead, SetActiveCanvas is called explicitly when a tab is added
        return;
    }

    if (canvasEntityId.IsValid() && canvasEntityId == m_activeCanvasEntityId)
    {
        // Nothing else to do. This occurs when a tab is clicked, but the active canvas cannot be changed so the current tab is reverted
        // back to the tab of the active canvas
        return;
    }

    if (!CanChangeActiveCanvas())
    {
        // Set the tab back to that of the active canvas
        int activeCanvasIndex = GetTabIndexForCanvasEntityId(m_activeCanvasEntityId);
        m_canvasTabBar->setCurrentIndex(activeCanvasIndex);

        QMessageBox::information(this,
            tr("Running Slice Operations"),
            tr("The current UI canvas is still running slice operations. Please wait until complete before changing tabs."));

        return;
    }

    SetActiveCanvas(canvasEntityId);
}

void EditorWindow::OnCanvasTabContextMenuRequested(const QPoint &point)
{
    int tabIndex = m_canvasTabBar->tabAt(point);

    if (tabIndex >= 0)
    {
        AZ::EntityId canvasEntityId = GetCanvasEntityIdForTabIndex(tabIndex);

        QMenu menu(this);
        menu.addAction(CreateSaveCanvasAction(canvasEntityId, true));
        menu.addAction(CreateSaveCanvasAsAction(canvasEntityId, true));
        menu.addAction(CreateSaveAllCanvasesAction(true));
        menu.addSeparator();
        menu.addAction(CreateCloseCanvasAction(canvasEntityId, true));
        menu.addAction(CreateCloseAllCanvasesAction(true));
        menu.addAction(CreateCloseAllOtherCanvasesAction(canvasEntityId, true));
        menu.addSeparator();

        QAction* action = new QAction("Copy Full Path", this);
        UiCanvasMetadata *canvasMetadata = GetCanvasMetadata(canvasEntityId);
        action->setEnabled(canvasMetadata && !canvasMetadata->m_canvasSourceAssetPathname.empty());
        QObject::connect(action,
            &QAction::triggered,
            this,
            [this, canvasEntityId](bool checked)
        {
            UiCanvasMetadata *canvasMetadata = GetCanvasMetadata(canvasEntityId);
            AZ_Assert(canvasMetadata, "Canvas metadata not found");
            if (canvasMetadata)
            {
                QApplication::clipboard()->setText(canvasMetadata->m_canvasSourceAssetPathname.c_str());
            }
        });
        menu.addAction(action);

        menu.exec(m_canvasTabBar->mapToGlobal(point));
    }
    else
    {
        if (m_canvasMetadataMap.size() > 0)
        {
            QMenu menu(this);
            menu.addAction(CreateSaveAllCanvasesAction(true));
            menu.addSeparator();
            menu.addAction(CreateCloseAllCanvasesAction(true));

            menu.exec(m_canvasTabBar->mapToGlobal(point));
        }
    }
}

void EditorWindow::SaveModeSettings(UiEditorMode mode, bool syncSettings)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, AZ_QCOREAPPLICATION_SETTINGS_ORGANIZATION_NAME);
    settings.beginGroup(UICANVASEDITOR_NAME_SHORT);

    if (mode == UiEditorMode::Edit)
    {
        // save the edit mode state
        settings.setValue(UICANVASEDITOR_SETTINGS_EDIT_MODE_STATE_KEY, saveState(UICANVASEDITOR_SETTINGS_WINDOW_STATE_VERSION));
        settings.setValue(UICANVASEDITOR_SETTINGS_EDIT_MODE_GEOM_KEY, saveGeometry());
    }
    else
    {
        // save the preview mode state
        settings.setValue(UICANVASEDITOR_SETTINGS_PREVIEW_MODE_STATE_KEY, saveState(UICANVASEDITOR_SETTINGS_WINDOW_STATE_VERSION));
        settings.setValue(UICANVASEDITOR_SETTINGS_PREVIEW_MODE_GEOM_KEY, saveGeometry());
    }

    settings.endGroup();    // UI canvas editor

    if (syncSettings)
    {
        settings.sync();
    }
}

void EditorWindow::RestoreModeSettings(UiEditorMode mode)
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, AZ_QCOREAPPLICATION_SETTINGS_ORGANIZATION_NAME);
    settings.beginGroup(UICANVASEDITOR_NAME_SHORT);

    if (mode == UiEditorMode::Edit)
    {
        // restore the edit mode state
        restoreState(settings.value(UICANVASEDITOR_SETTINGS_EDIT_MODE_STATE_KEY).toByteArray(), UICANVASEDITOR_SETTINGS_WINDOW_STATE_VERSION);
        restoreGeometry(settings.value(UICANVASEDITOR_SETTINGS_EDIT_MODE_GEOM_KEY).toByteArray());
    }
    else
    {
        // restore the preview mode state
        bool stateRestored = restoreState(settings.value(UICANVASEDITOR_SETTINGS_PREVIEW_MODE_STATE_KEY).toByteArray(), UICANVASEDITOR_SETTINGS_WINDOW_STATE_VERSION);
        bool geomRestored = restoreGeometry(settings.value(UICANVASEDITOR_SETTINGS_PREVIEW_MODE_GEOM_KEY).toByteArray());

        // if either of the above failed then manually hide and show widgets,
        // this will happen the first time someone uses preview mode
        if (!stateRestored || !geomRestored)
        {
            m_hierarchyDockWidget->hide();
            m_propertiesDockWidget->hide();
            m_animationDockWidget->hide();
            m_mainToolbar->hide();
            m_modeToolbar->hide();
            m_enterPreviewToolbar->hide();

            m_previewToolbar->show();
            m_previewActionLogDockWidget->show();
            m_previewAnimationListDockWidget->show();
        }
    }

    settings.endGroup();    // UI canvas editor
}

static const char* UIEDITOR_UNLOAD_SAVED_CANVAS_METRIC_EVENT_NAME = "UiEditorUnloadSavedCanvas";
static const char* UIEDITOR_CANVAS_ID_ATTRIBUTE_NAME = "CanvasId";
static const char* UIEDITOR_CANVAS_WIDTH_METRIC_NAME = "CanvasWidth";
static const char* UIEDITOR_CANVAS_HEIGHT_METRIC_NAME = "CanvasHeight";
static const char* UIEDITOR_CANVAS_MAX_HIERARCHY_DEPTH_METRIC_NAME = "MaxHierarchyDepth";
static const char* UIEDITOR_CANVAS_NUM_ELEMENT_METRIC_NAME = "NumElement";
static const char* UIEDITOR_CANVAS_NUM_ELEMENTS_WITH_COMPONENT_PREFIX_METRIC_NAME = "Num";
static const char* UIEDITOR_CANVAS_NUM_ELEMENTS_WITH_CUSTOM_COMPONENT_METRIC_NAME = "NumCustomElement";
static const char* UIEDITOR_CANVAS_NUM_UNIQUE_CUSTOM_COMPONENT_NAME = "NumUniqueCustomComponent";
static const char* UIEDITOR_CANVAS_NUM_AVAILABLE_CUSTOM_COMPONENT_NAME = "NumAvailableCustomComponent";
static const char* UIEDITOR_CANVAS_NUM_ANCHOR_PRESETS_ATTRIBUTE_NAME = "NumAnchorPreset";
static const char* UIEDITOR_CANVAS_NUM_ANCHOR_CUSTOM_ATTRIBUTE_NAME = "NumAnchorCustom";
static const char* UIEDITOR_CANVAS_NUM_PIVOT_PRESETS_ATTRIBUTE_NAME = "NumPivotPreset";
static const char* UIEDITOR_CANVAS_NUM_PIVOT_CUSTOM_ATTRIBUTE_NAME = "NumPivotCustom";
static const char* UIEDITOR_CANVAS_NUM_ROTATED_ELEMENT_METRIC_NAME = "NumRotatedElement";
static const char* UIEDITOR_CANVAS_NUM_SCALED_ELEMENT_METRIC_NAME = "NumScaledElement";
static const char* UIEDITOR_CANVAS_NUM_SCALE_TO_DEVICE_ELEMENT_METRIC_NAME = "NumScaleToDeviceElement";

void EditorWindow::SubmitUnloadSavedCanvasMetricEvent(AZ::EntityId canvasEntityId)
{
    // Create an unload canvas event
    auto eventId = LyMetrics_CreateEvent(UIEDITOR_UNLOAD_SAVED_CANVAS_METRIC_EVENT_NAME);

    // Add unique canvas Id attribute
    AZ::u64 uniqueId = 0;
    EBUS_EVENT_ID_RESULT(uniqueId, canvasEntityId, UiCanvasBus, GetUniqueCanvasId);
    QString uniqueIdString;
    uniqueIdString.setNum(uniqueId);
    LyMetrics_AddAttribute(eventId, UIEDITOR_CANVAS_ID_ATTRIBUTE_NAME, qPrintable(uniqueIdString));

    // Add canvas size metric
    AZ::Vector2 canvasSize;
    EBUS_EVENT_ID_RESULT(canvasSize, canvasEntityId, UiCanvasBus, GetCanvasSize);
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_WIDTH_METRIC_NAME, canvasSize.GetX());
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_HEIGHT_METRIC_NAME, canvasSize.GetY());

    // Add max hierarchy depth metric
    LyShine::EntityArray childElements;
    EBUS_EVENT_ID_RESULT(childElements, canvasEntityId, UiCanvasBus, GetChildElements);
    int maxDepth = GetCanvasMaxHierarchyDepth(childElements);
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_MAX_HIERARCHY_DEPTH_METRIC_NAME, maxDepth);

    // Get a list of the component types that can be added to a UI element
    // The ComponentTypeData struct has a flag to say whether the component is an LyShine component
    AZStd::vector<ComponentHelpers::ComponentTypeData> uiComponentTypes = ComponentHelpers::GetAllComponentTypesThatCanAppearInAddComponentMenu();

    // Make a list of all elements of this canvas
    LyShine::EntityArray allElements;
    EBUS_EVENT_ID(canvasEntityId, UiCanvasBus, FindElements,
        [](const AZ::Entity* entity) { return true; },
        allElements);

    // Add total number of elements metric
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_ELEMENT_METRIC_NAME, allElements.size());

    std::vector<int> numElementsWithComponent(uiComponentTypes.size(), 0);
    int numElementsWithCustomComponent = 0;
    int numCustomComponentsAvailable = 0;
    for (int i = 0; i < uiComponentTypes.size(); i++)
    {
        if (!uiComponentTypes[i].isLyShineComponent)
        {
            ++numCustomComponentsAvailable;
        }
    }
    std::vector<int> numElementsWithAnchorPreset(AnchorPresets::PresetIndexCount, 0);
    int numElementsWithCustomAnchors = 0;
    std::vector<int> numElementsWithPivotPreset(PivotPresets::PresetIndexCount, 0);
    int numElementsWithCustomPivot = 0;
    int numRotatedElements = 0;
    int numScaledElements = 0;
    int numScaleToDeviceElements = 0;

    for (auto entity : allElements)
    {
        // Check which components this element has
        bool elementHasCustomComponent = false;
        for (int i = 0; i < uiComponentTypes.size(); i++)
        {
            if (entity->FindComponent(uiComponentTypes[i].classData->m_typeId))
            {
                numElementsWithComponent[i]++;

                if (!uiComponentTypes[i].isLyShineComponent)
                {
                    elementHasCustomComponent = true;
                }
            }
        }

        if (elementHasCustomComponent)
        {
            numElementsWithCustomComponent++;
        }

        // Check if this element is controlled by its parent
        bool isControlledByParent = false;
        AZ::Entity* parentElement = EntityHelpers::GetParentElement(entity);
        if (parentElement)
        {
            EBUS_EVENT_ID_RESULT(isControlledByParent, parentElement->GetId(), UiLayoutBus, IsControllingChild, entity->GetId());
        }

        if (!isControlledByParent)
        {
            // Check if this element is scaled
            AZ::Vector2 scale(1.0f, 1.0f);
            EBUS_EVENT_ID_RESULT(scale, entity->GetId(), UiTransformBus, GetScale);
            if (scale.GetX() != 1.0f || scale.GetY() != 1.0f)
            {
                numScaledElements++;
            }

            // Check if this element is rotated
            float rotation = 0.0f;
            EBUS_EVENT_ID_RESULT(rotation, entity->GetId(), UiTransformBus, GetZRotation);
            if (rotation != 0.0f)
            {
                numRotatedElements++;
            }

            // Check if this element is using an anchor preset
            UiTransform2dInterface::Anchors anchors;
            EBUS_EVENT_ID_RESULT(anchors, entity->GetId(), UiTransform2dBus, GetAnchors);
            AZ::Vector4 anchorVect(anchors.m_left, anchors.m_top, anchors.m_right, anchors.m_bottom);
            int anchorPresetIndex = AnchorPresets::AnchorToPresetIndex(anchorVect);
            if (anchorPresetIndex >= 0)
            {
                numElementsWithAnchorPreset[anchorPresetIndex]++;
            }
            else
            {
                numElementsWithCustomAnchors++;
            }

            // Check if this element is using a pivot preset
            AZ::Vector2 pivot;
            EBUS_EVENT_ID_RESULT(pivot, entity->GetId(), UiTransformBus, GetPivot);
            AZ::Vector2 pivotVect(pivot.GetX(), pivot.GetY());
            int pivotPresetIndex = PivotPresets::PivotToPresetIndex(pivotVect);
            if (pivotPresetIndex >= 0)
            {
                numElementsWithPivotPreset[pivotPresetIndex]++;
            }
            else
            {
                numElementsWithCustomPivot++;
            }
        }

        // Check if this element is scaled to device
        bool scaleToDevice = false;
        EBUS_EVENT_ID_RESULT(scaleToDevice, entity->GetId(), UiTransformBus, GetScaleToDevice);
        if (scaleToDevice)
        {
            numScaleToDeviceElements++;
        }
    }

    // Add metric for each internal component representing the number of elements having that component
    int numCustomComponentsUsed = 0;
    for (int i = 0; i < uiComponentTypes.size(); i++)
    {
        auto& componentType = uiComponentTypes[i];
        if (componentType.isLyShineComponent)
        {
            AZ::Edit::ClassData* editInfo = componentType.classData->m_editData;
            if (editInfo)
            {
                int count = numElementsWithComponent[i];
                AZStd::string metricName(UIEDITOR_CANVAS_NUM_ELEMENTS_WITH_COMPONENT_PREFIX_METRIC_NAME);
                metricName += editInfo->m_name;
                LyMetrics_AddMetric(eventId, metricName.c_str(), count);
            }
        }
        else
        {
            numCustomComponentsUsed++;
        }
    }

    // Add metric for the number of elements that have a custom component
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_ELEMENTS_WITH_CUSTOM_COMPONENT_METRIC_NAME, numElementsWithCustomComponent);

    // Add metric for the number of unique custom components used
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_UNIQUE_CUSTOM_COMPONENT_NAME, numCustomComponentsUsed);

    // Add a metric for the number of available custom components
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_AVAILABLE_CUSTOM_COMPONENT_NAME, numCustomComponentsAvailable);

    // Construct a string representing the number of elements that use each anchor preset e.g. {0, 1, 6, 20, ...}
    QString anchorPresetString("{");
    for (int i = 0; i < AnchorPresets::PresetIndexCount; i++)
    {
        anchorPresetString.append(QString::number(numElementsWithAnchorPreset[i]));
        if (i < AnchorPresets::PresetIndexCount - 1)
        {
            anchorPresetString.append(", ");
        }
    }
    anchorPresetString.append("}");

    // Add attribute representing the number of elements using each anchor preset
    LyMetrics_AddAttribute(eventId, UIEDITOR_CANVAS_NUM_ANCHOR_PRESETS_ATTRIBUTE_NAME, qPrintable(anchorPresetString));

    // Add metric representing the number of elements with their anchors set to a custom value
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_ANCHOR_CUSTOM_ATTRIBUTE_NAME, numElementsWithCustomAnchors);

    // Construct a string representing the number of elements that use each pivot preset e.g. {0, 1, 6, 20, ...}
    QString pivotPresetString("{");
    for (int i = 0; i < PivotPresets::PresetIndexCount; i++)
    {
        pivotPresetString.append(QString::number(numElementsWithPivotPreset[i]));
        if (i < PivotPresets::PresetIndexCount - 1)
        {
            pivotPresetString.append(", ");
        }
    }
    pivotPresetString.append("}");

    // Add attribute representing the number of elements using each pivot preset
    LyMetrics_AddAttribute(eventId, UIEDITOR_CANVAS_NUM_PIVOT_PRESETS_ATTRIBUTE_NAME, qPrintable(pivotPresetString));

    // Add metric representing the number of elements with their pivot set to a custom value
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_PIVOT_CUSTOM_ATTRIBUTE_NAME, numElementsWithCustomPivot);

    // Add number of rotated elements metric
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_ROTATED_ELEMENT_METRIC_NAME, numRotatedElements);

    // Add number of scaled elements metric
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_SCALED_ELEMENT_METRIC_NAME, numScaledElements);

    // Add metric representing the number of elements that are scaled to device
    LyMetrics_AddMetric(eventId, UIEDITOR_CANVAS_NUM_SCALE_TO_DEVICE_ELEMENT_METRIC_NAME, numScaleToDeviceElements);

    // Submit the event
    LyMetrics_SubmitEvent(eventId);
}

int EditorWindow::GetCanvasMaxHierarchyDepth(const LyShine::EntityArray& rootChildElements)
{
    int depth = 0;

    if (rootChildElements.empty())
    {
        return depth;
    }

    int numChildrenCurLevel = rootChildElements.size();
    int numChildrenNextLevel = 0;
    std::list<AZ::Entity*> elementList(rootChildElements.begin(), rootChildElements.end());
    while (!elementList.empty())
    {
        auto& entity = elementList.front();

        LyShine::EntityArray childElements;
        EBUS_EVENT_ID_RESULT(childElements, entity->GetId(), UiElementBus, GetChildElements);
        if (!childElements.empty())
        {
            elementList.insert(elementList.end(), childElements.begin(), childElements.end());
            numChildrenNextLevel += childElements.size();
        }

        elementList.pop_front();
        numChildrenCurLevel--;

        if (numChildrenCurLevel == 0)
        {
            depth++;
            numChildrenCurLevel = numChildrenNextLevel;
            numChildrenNextLevel = 0;
        }
    }

    return depth;
}

void EditorWindow::DeleteSliceLibraryTree()
{
    // this just deletes the tree so that we know it is dirty
    if (m_sliceLibraryTree)
    {
        delete m_sliceLibraryTree;
        m_sliceLibraryTree = nullptr;
    }
}

bool EditorWindow::event(QEvent* ev)
{
    if (ev->type() == QEvent::ShortcutOverride)
    {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(ev);
        QKeySequence keySequence(keyEvent->key() | keyEvent->modifiers());

        if (keySequence == UICANVASEDITOR_COORDINATE_SYSTEM_CYCLE_SHORTCUT_KEY_SEQUENCE)
        {
            ev->accept();
            return true;
        }
        else if (keySequence == UICANVASEDITOR_SNAP_TO_GRID_TOGGLE_SHORTCUT_KEY_SEQUENCE)
        {
            ev->accept();
            return true;
        }
    }

    return QMainWindow::event(ev);
}

void EditorWindow::keyReleaseEvent(QKeyEvent* ev)
{
    QKeySequence keySequence(ev->key() | ev->modifiers());

    if (keySequence == UICANVASEDITOR_COORDINATE_SYSTEM_CYCLE_SHORTCUT_KEY_SEQUENCE)
    {
        SignalCoordinateSystemCycle();
    }
    else if (keySequence == UICANVASEDITOR_SNAP_TO_GRID_TOGGLE_SHORTCUT_KEY_SEQUENCE)
    {
        SignalSnapToGridToggle();
    }
}

void EditorWindow::paintEvent(QPaintEvent* paintEvent)
{
    QMainWindow::paintEvent(paintEvent);

    if (m_viewport)
    {
        m_viewport->Refresh();
    }
}

void EditorWindow::closeEvent(QCloseEvent* closeEvent)
{
    if (!CanExitNow())
    {
        // Nothing to do.
        closeEvent->ignore();
        return;
    }

    // Save the current window state
    SaveEditorWindowSettings();

    QMainWindow::closeEvent(closeEvent);
}

#include <EditorWindow.moc>
