#include <iostream>
#include <sstream>
#include <mongo/client/dbclient.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

using namespace mongo;

static const long PAGE_SIZE = sysconf(_SC_PAGESIZE);

static bool isIncore(unsigned char* addr, int32_t length, set<void*>& cachedPages) {

    unsigned char* roundedAddr = (unsigned char*)((long)addr / PAGE_SIZE * PAGE_SIZE);

    long overhead = (long)addr - (long)roundedAddr;
    if (overhead < 0 || overhead >= PAGE_SIZE) {
        throw logic_error("illegal overhead. must not happen");
    }

    length += overhead;

    int size = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned char vec[size];

    if (mincore(roundedAddr, length, vec) != 0) {
        ostringstream s;
        s << "mincore @" << (void*)roundedAddr << " failed: " << strerror(errno);
        throw runtime_error(s.str());
    }

    // is every page in cache?
    for (int i=0; i<size; i++) {
        if((vec[0] & 1) != 1) {
            return false;
        }
    }

    long pageAddr = (long)addr / PAGE_SIZE;
    for (int i=0; i<size; i++) {
        cachedPages.insert((void*)((pageAddr + i)*PAGE_SIZE));
    }

    return true;
}

static string resolveFilepath(const string& path, const string& database, int number, bool directoryPerDb) {
    if (number < 0) {
        throw invalid_argument("negative number");
    }
    ostringstream s;
    s << path << "/";
    if (directoryPerDb) {
        s << database << "/";
    }
    s << database << "." << number;
    return s.str();
}

class mapping {
public:
    void close() {
        if (data != NULL) {
            if (::munmap(data, size) == -1) {
                throw runtime_error(string("munmap failed: ") + string(strerror(errno)));
            }
            data = NULL;
            size = 0;
        }
        if (fd > 0) {
            if (::close(fd) == -1) {
                throw runtime_error(string("failed to close file: ") + string(strerror(errno)));
            }
            fd = 0;
        }
    }

    unsigned char* getData() const {
        return data;
    }

    bool mapped() const {
        return (fd != 0);
    }

    void map(const string& filepath) {
        fd = open(filepath.c_str(), O_RDONLY | O_LARGEFILE);
        if (fd < 0) {
            throw runtime_error(string("failed to open ") + filepath + ": " + string(strerror(errno)));
        }

        struct stat stbuf;
        if (fstat(fd, &stbuf) == -1) {
            ostringstream s;
            s << "failed to stat fd " << fd << ": " << strerror(errno);
            throw runtime_error(s.str());
        }
        size = stbuf.st_size;

        data = (unsigned char*)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            throw runtime_error(string("mmap failed ") + strerror(errno));
        }

        /*
         * Heisenberg compensator
         *
         * Turn-off read-ahead, read-behind
         *
         * From the Linux 3.x kernel documentation:
         *   behavior values:
         *    MADV_NORMAL - the default behavior is to read clusters.  This
         *                results in some read-ahead and read-behind.
         *    MADV_RANDOM - the system should read the minimum amount of data
         *                on any access, since it is unlikely that the appli-
         *                cation will need more than what it asks for.
         */
        if (madvise(data, size, MADV_RANDOM) != 0) {
            throw runtime_error(string("madvise failed ") + strerror(errno));
        }
    }

    mapping(int _fd=0, unsigned char* _data=NULL, off_t _size=0) : fd(_fd), data(_data), size(_size) {
    }

    virtual ~mapping() {
        close();
    }

private:
    int fd;
    unsigned char* data;
    off_t size;
};

static void scan(DBClientConnection& c, const string& dbpath, const string& db, const string& collection, const bool directoryPerDb) {

    set<void*> cachedPages;
    unsigned long total = 0;
    unsigned long cachedData = 0;
    unsigned long cachedObjects = 0;
    unsigned long uncachedObjects = 0;

    const BSONObj fields = BSONObjBuilder().append("_id", 1).obj();
    const Query query = BSONObjBuilder()
                  .append("$query", BSONObj())
                  .append("$orderby", fields)
                  .append("$hint", fields)
                  .append("$showDiskLoc", 1).obj();

    const string ns = db + "." + collection;

    map<string, mapping> mappings;

    auto_ptr<DBClientCursor> cursor = c.query(ns, query, 0, 0, &fields, QueryOption_SlaveOk, 0);

    while (cursor->more()) {
        const BSONObj obj = cursor->next();
        BSONElement id;
        if (!obj.getObjectID(id)) {
            throw runtime_error(string("object id not found: ") + obj.toString());
        }

        const BSONObj& diskLoc = obj.getObjectField("$diskLoc");
        if (diskLoc.isEmpty()) {
            ostringstream s;
            s << "got not disk location: " << obj << " (total: " << total << ")";
            s << " see: https://jira.mongodb.org/browse/SERVER-5372" << endl;
            throw runtime_error(s.str());
        }

        int file = diskLoc["file"].Int();
        const int offset = diskLoc["offset"].Int();

        const string filepath = resolveFilepath(dbpath, db, file, directoryPerDb);
        mapping& mapping = mappings[filepath];
        if (!mapping.mapped()) {
            mapping.map(filepath);
        }

        unsigned char* data = mapping.getData();

        static const int headerSize = 16; // found by reverse engineering

        if (!isIncore(data + offset, sizeof(int32_t), cachedPages)) {
            uncachedObjects++;
        }
        else {
            int32_t docSize = *((int32_t*)&(data[offset]));
            int32_t objSize = *((int32_t*)&(data[offset + headerSize]));
            if (docSize <= 1 || docSize > 1024*1024) {
                ostringstream s;
                s << "illegal document size: " << objSize;
                throw runtime_error(s.str());
            }
            if (objSize >= docSize || objSize <= 1 || objSize > 1024*1024) {
                ostringstream s;
                s << "illegal object size: " << objSize;
                throw runtime_error(s.str());
            }

            if (isIncore(data + offset, docSize, cachedPages)) {
                const int endOffset = offset + headerSize + objSize - 1;
                // validate BSON consistency by checking the last byte which must be 0 according to the specs
                // since the entire document seems to be incore we assume not to cause paging by accessing the data here
                const unsigned char* end = data + endOffset;
                if (*end != '\0') {
                    ostringstream s;
                    s << "illegal BSON last byte: ";
                    s << hex << (short)(*end) << " (hex)" << dec;
                    s << " for " << obj << " with size " << objSize;
                    s << ". Expected is a zero byte. Either the mongo server version is not supported or the data is corrupt.";
                    throw runtime_error(s.str());
                }
                cachedData += docSize;
                cachedObjects++;
            }
            else {
                uncachedObjects++;
            }
        }

        if (++total % 10000 == 0) {
            printf("\r%10s | %15s | %10lu | %10lu ", db.c_str(), collection.c_str(), total, cachedObjects);
            fflush(stdout);
        }

    }
    mappings.clear();

    float cacheRate = 100.0 * cachedObjects / (uncachedObjects + cachedObjects);
    unsigned long cachedPagesBytes = cachedPages.size()*PAGE_SIZE;
    float cacheSizeRatio = 100.0 * cachedData / cachedPagesBytes;
    printf("\r%10s | %15s | %10lu | %10lu | %5.1f %% | %6lu | %10lu | %10lu | %5.1f %%\n",
           db.c_str(),
           collection.c_str(),
           total,
           cachedObjects,
           cacheRate,
           cachedPages.size(),
           cachedPagesBytes,
           cachedData,
           cacheSizeRatio);
}

static list<string> getCollectionNames(DBClientConnection& c, const string& db ) {
    list<string> names;

    string ns = db + ".system.namespaces";
    // query (const string &ns, Query query=Query(), int nToReturn=0, int nToSkip=0, const BSONObj *fieldsToReturn=0, int queryOptions=0, int batchSize=0)
    auto_ptr<DBClientCursor> cursor = c.query( ns.c_str() , BSONObj(), 0, 0, 0, QueryOption_SlaveOk );
    while ( cursor->more() ) {
        string name = cursor->next()["name"].valuestr();
        if ( name.find( "$" ) != string::npos )
            continue;
        names.push_back( name );
    }
    return names;
}

void checkVersion(DBClientConnection& c) {
    cout << "checking server version" << endl;
    BSONObj info;

    if (!c.runCommand("admin", BSON("buildinfo" << 1), info)) {
        throw runtime_error("failed to query server buildinfo");
    }

    const string version = info["version"].String();

    const vector<BSONElement>& versionArray = info["versionArray"].Array();
    const int major = versionArray[0].Int();
    const int minor = versionArray[1].Int();

    if (major < 2 || (major == 2 && minor < 6)) {
        ostringstream s;
        s << "version " << version << " is not supported. At least 2.6 is required" << endl;
        s << " see: https://jira.mongodb.org/browse/SERVER-5372" << endl;
        throw runtime_error(s.str());
    }

    cout << "server version " << version << " OK" << endl;
}

int main(int args, const char* argv[]) {
    if (args < 2) {
        cerr << "usage: " << argv[0] << " [--directoryPerDb] <dbpath> [<host:port>]" << endl;
        return 1;
    }
    try {
        bool directoryPerDb = false;
        string dbpath;
        string host = "localhost";
        int options = 0;
        for (int i=1; i<args; i++) {
            const string arg = argv[i];
            if (arg.size() > 2 && arg.substr(0, 2) == "--") {
                options++;
                if (arg == "--directoryPerDb") {
                    directoryPerDb = true;
                }
                else {
                    throw runtime_error("unexpected option: " + arg);
                }
            }
            else {
                int argNr = i - options;
                switch(argNr) {
                case 1:
                    dbpath = arg;
                    break;
                case 2:
                    host = arg;
                    break;
                default:
                    throw runtime_error("unexpected argument: " + arg);
                }
            }
        }

        cout << "using dbpath '" << dbpath << "'" << endl;

        DBClientConnection c;
        c.connect(host);

        cout << "connected to " << host << endl;

        checkVersion(c);

        cout << endl;
        cout << "  database |   collection    |   objects  |    cached  |  rate   |  pages | pages size |  objs size |  ratio" << endl;
        cout << "-------------------------------------------------------------------------------------------------------------" << endl;

        list<string> databases = c.getDatabaseNames();
        list<string>::iterator it;
        for (it = databases.begin(); it != databases.end(); it++) {
            const string& db = *it;
            if (db == "local" || db == "admin") {
                continue;
            }

            list<string> collections = getCollectionNames(c, db);
            list<string>::iterator collIt;
            for (collIt = collections.begin(); collIt != collections.end(); collIt++) {
                const string& ns = *collIt;
                if (ns.empty()) {
                    throw runtime_error("got empty collection name");
                }

                const string collection = ns.substr(ns.find(".") + 1);
                if (collection.find("system.") == 0) {
                    continue;
                }
                scan(c, dbpath, db, collection, directoryPerDb);
            }
        }

    }
    catch( DBException &e ) {
        cout << "caught " << e.what() << endl;
        return -1;
    }
    return 0;
}
