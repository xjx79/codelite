#include "refactoring_storage.h"
#include "event_notifier.h"
#include <wx/filename.h>
#include "cppwordscanner.h"
#include "refactorengine.h"

class CppTokenCacheMakerThread : public wxThread
{
protected:
    RefactoringStorage* m_storage;
    wxString m_workspaceFile;
    wxFileList_t m_files;

public:
    CppTokenCacheMakerThread(RefactoringStorage* storage, const wxString &workspaceFile, const wxFileList_t& files)
        : wxThread(wxTHREAD_DETACHED)
        , m_storage(storage)
        , m_workspaceFile(workspaceFile.c_str()) {
        m_files.insert(m_files.end(), files.begin(), files.end());
    }

    virtual ~CppTokenCacheMakerThread()
    {}

    void *Entry() {
        RefactoringStorage storage;
        storage.Open(m_workspaceFile);
        storage.m_cacheStatus = RefactoringStorage::CACHE_READY;

        wxCommandEvent evtStatus1(wxEVT_REFACTORING_ENGINE_CACHE_INITIALIZING);
        evtStatus1.SetInt(0);
        evtStatus1.SetString( m_workspaceFile );
        EventNotifier::Get()->AddPendingEvent( evtStatus1 );

        size_t count = 0;
        
        storage.Begin();
        wxFileList_t::const_iterator iter = m_files.begin();
        for(; iter != m_files.end(); ++iter ) {
            ++count;
            if ( count % 100 == 0 ) {
                storage.Commit();
                storage.Begin();
            }
            
            wxString fullpath = iter->GetFullPath();
            if ( !storage.IsFileUpToDate(fullpath) ) {
                CppWordScanner scanner( fullpath );
                CppToken::List_t tokens = scanner.tokenize();
                storage.StoreTokens( fullpath, tokens, false );
            }
        }
        storage.Commit();
        
        wxCommandEvent evtStatus2(wxEVT_REFACTORING_ENGINE_CACHE_INITIALIZING);
        evtStatus2.SetInt(100);
        evtStatus2.SetString( m_workspaceFile );
        EventNotifier::Get()->AddPendingEvent( evtStatus2 );
        return NULL;
    }
};

RefactoringStorage::RefactoringStorage()
    : m_cacheStatus(CACHE_NOT_READY)
{
    // wxEVT_WORKSPACE_LOADED
    EventNotifier::Get()->Connect(3452, wxCommandEventHandler(RefactoringStorage::OnWorkspaceLoaded), NULL, this);
    // wxEVT_WORKSPACE_CLOSED
    EventNotifier::Get()->Connect(3454, wxCommandEventHandler(RefactoringStorage::OnWorkspaceClosed), NULL, this);
    EventNotifier::Get()->Connect(wxEVT_REFACTORING_ENGINE_CACHE_INITIALIZING, wxCommandEventHandler(RefactoringStorage::OnThreadStatus), NULL, this);
}

RefactoringStorage::~RefactoringStorage()
{
    EventNotifier::Get()->Disconnect(3452, wxCommandEventHandler(RefactoringStorage::OnWorkspaceLoaded), NULL, this);
    EventNotifier::Get()->Disconnect(3454, wxCommandEventHandler(RefactoringStorage::OnWorkspaceClosed), NULL, this);
    EventNotifier::Get()->Disconnect(wxEVT_REFACTORING_ENGINE_CACHE_INITIALIZING, wxCommandEventHandler(RefactoringStorage::OnThreadStatus), NULL, this);
}

void RefactoringStorage::StoreTokens(const wxString& filename, const CppToken::List_t& tokens, bool startTx)
{
    if ( !IsCacheReady() ) {
        return;
    }

    if ( !m_db.IsOpen() )
        return;

    try {
        if ( startTx )  { 
            Begin();
        }
        
        DoDeleteFile(filename);

        CppToken::List_t::const_iterator iter = tokens.begin();
        for(; iter != tokens.end(); ++iter ) {
            iter->store( &m_db );
        }

        DoUpdateFileTimestamp(filename);
        
        if ( startTx )  { 
            Commit();
        }

    } catch (wxSQLite3Exception &e) {
        if ( startTx ) {
            Rollback();
        }
        wxUnusedVar(e);
    }
}

void RefactoringStorage::Open(const wxString& workspacePath)
{
    wxString cache_db;
    wxFileName fnWorkspace( workspacePath );
    cache_db << fnWorkspace.GetPath() << "/.codelite";

    {
        wxLogNull nolog;
        ::wxMkdir( cache_db );
    }

    cache_db << "/refactoring.db";
    if ( m_db.IsOpen() ) {
        m_db.Close();
    }

    m_db.Open(cache_db);
    m_cacheDb = cache_db;

    // Create the schema
    try {
        m_db.ExecuteUpdate("PRAGMA journal_mode= OFF");

        m_db.ExecuteUpdate("PRAGMA synchronous = OFF");

        m_db.ExecuteUpdate("PRAGMA temp_store = MEMORY");

        m_db.ExecuteUpdate(
            "create table if not exists TOKENS_TABLE(ID INTEGER PRIMARY KEY AUTOINCREMENT,"
            "                                        NAME VARCHAR(128),"
            "                                        OFFSET INTEGER,"
            "                                        FILE_NAME VARCHAR(256),"
            "                                        LINE_NUMBER INTEGER)");
        m_db.ExecuteUpdate("create index if not exists TOKENS_TABLE_IDX1 on TOKENS_TABLE(NAME)");
        m_db.ExecuteUpdate("create index if not exists TOKENS_TABLE_IDX2 on TOKENS_TABLE(FILE_NAME)");

        m_db.ExecuteUpdate("create table if not exists FILES (ID INTEGER PRIMARY KEY AUTOINCREMENT, FILE_NAME VARCHAR(256), LAST_UPDATED INTEGER)");
        m_db.ExecuteUpdate("create unique index if not exists FILES_IDX1 on FILES(FILE_NAME)");

    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
}

void RefactoringStorage::DoUpdateFileTimestamp(const wxString& filename)
{
    try {
        wxSQLite3Statement st = m_db.PrepareStatement("REPLACE INTO FILES (ID, FILE_NAME, LAST_UPDATED) VALUES ( NULL, ?, ?)");
        st.Bind(1, filename);
        st.Bind(2, (int)time(NULL));
        st.ExecuteUpdate();

    } catch (wxSQLite3Exception &e) {
        wxUnusedVar( e );
    }
}

void RefactoringStorage::OnWorkspaceLoaded(wxCommandEvent& e)
{
    e.Skip();
    m_workspaceFile = e.GetString();
    m_cacheStatus = CACHE_NOT_READY;
    Open( m_workspaceFile );
}

void RefactoringStorage::DoDeleteFile(const wxString& filename)
{
    try {

        // delete fhe file entry
        wxSQLite3Statement st = m_db.PrepareStatement("DELETE FROM FILES WHERE FILE_NAME=?");
        st.Bind(1, filename);
        st.ExecuteUpdate();

        // remove all tokens
        wxSQLite3Statement st3 = m_db.PrepareStatement("delete from TOKENS_TABLE where FILE_NAME=?");
        st3.Bind(1, filename);
        st3.ExecuteUpdate();

    } catch (wxSQLite3Exception &e) {
        wxUnusedVar( e );
    }
}

void RefactoringStorage::Match(const wxString& symname, const wxString& filename, CppTokensMap& matches)
{
    CppToken::List_t list;
    if ( IsCacheReady() && IsFileUpToDate(filename) ) {
        // load tokens for a file from the database
        list = CppToken::loadByNameAndFile(&m_db, symname, filename);
        matches.addToken( symname, list );

    } else {

        CppWordScanner tmpScanner(filename);
        tmpScanner.Match(symname, matches);
        matches.findTokens(symname, list);
        
        // update the cache
        if ( !list.empty() && IsCacheReady()) {
            StoreTokens(symname, list, true);
        }
    }
}

bool RefactoringStorage::IsFileUpToDate(const wxString& filename)
{
    if ( !wxFileName(filename).FileExists() ) {
        return true;
    }

    time_t last_updated_in_db = 0;
    time_t lastModified = wxFileName(filename).GetModificationTime().GetTicks();
    try {
        wxString sql("SELECT LAST_UPDATED from FILES WHERE FILE_NAME=?");
        wxSQLite3Statement st = m_db.PrepareStatement(sql);
        st.Bind(1, filename);
        wxSQLite3ResultSet res = st.ExecuteQuery();
        if ( res.NextRow() ) {
            last_updated_in_db = res.GetInt(0);
        }
        return last_updated_in_db >= lastModified;
    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
    return false;
}

void RefactoringStorage::InitializeCache(const wxFileList_t& files)
{
    if ( m_cacheStatus == CACHE_NOT_READY ) {
        m_cacheStatus = CACHE_IN_PROGRESS;
        CppTokenCacheMakerThread *thr = new CppTokenCacheMakerThread(this, m_workspaceFile, files);
        thr->Create();
        thr->Run();
    }
}
wxFileList_t RefactoringStorage::FilterUpToDateFiles(const wxFileList_t& files)
{
    if ( !IsCacheReady() ) {
        return files;
    }
    wxFileList_t res;
    wxFileList_t::const_iterator iter = files.begin();
    for(; iter != files.end(); ++iter ) {
        if ( !IsFileUpToDate(iter->GetFullPath()) ) {
            res.push_back( *iter );
        }
    }
    return res;
}

CppToken::List_t RefactoringStorage::GetTokens(const wxString& symname)
{
    if ( !IsCacheReady() ) {
        return CppToken::List_t();
    }

    return CppToken::loadByName(&m_db, symname);
}

void RefactoringStorage::OnWorkspaceClosed(wxCommandEvent& e)
{
    e.Skip();
    m_cacheStatus = CACHE_NOT_READY;
    try {

        m_db.Close();
        m_workspaceFile.Clear();
        m_cacheDb.Clear();

    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
}

void RefactoringStorage::OnThreadStatus(wxCommandEvent& e)
{
    e.Skip();
    if ( e.GetInt() == 100 ) {
        // completed
        if ( e.GetString() == m_workspaceFile && m_cacheStatus == CACHE_IN_PROGRESS ) {
            // same file
            m_cacheStatus = CACHE_READY;
        }
    }
}

void RefactoringStorage::Begin()
{
    try {
        m_db.Begin();
    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
}

void RefactoringStorage::Commit()
{
    try {
        m_db.Commit();
    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
}

void RefactoringStorage::Rollback()
{
    try {
        m_db.Rollback();
    } catch (wxSQLite3Exception &e) {
        wxUnusedVar(e);
    }
}