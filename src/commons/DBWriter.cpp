#include "DBWriter.h"
#include "DBReader.h"
#include "Debug.h"
#include "Util.h"
#include "FileUtil.h"
#include "Concat.h"
#include "itoa.h"
#include "Timer.h"
#include "Parameters.h"

#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <unistd.h>

#ifdef OPENMP
#include <omp.h>
#endif

DBWriter::DBWriter(const char *dataFileName_, const char *indexFileName_, unsigned int threads, size_t mode, int dbtype)
        : threads(threads), mode(mode), dbtype(dbtype) {
    dataFileName = strdup(dataFileName_);
    indexFileName = strdup(indexFileName_);

    dataFiles = new FILE *[threads];
    dataFilesBuffer = new char *[threads];

    dataFileNames = new char *[threads];

    indexFiles = new FILE *[threads];
    indexFileNames = new char *[threads];
    compressedBuffers=NULL;
    compressedBufferSizes=NULL;
    if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0){
        compressedBuffers = new char*[threads];
        compressedBufferSizes = new size_t[threads];
        cstream = new ZSTD_CStream*[threads];
    }

    starts = new size_t[threads];
    std::fill(starts, starts + threads, 0);
    offsets = new size_t[threads];
    std::fill(offsets, offsets + threads, 0);
    if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0 ){
        datafileMode = "wb+";
    } else {
        datafileMode = "wb";
    }

    closed = true;
}

DBWriter::~DBWriter() {
    delete[] offsets;
    delete[] starts;
    delete[] indexFileNames;
    delete[] indexFiles;
    delete[] dataFileNames;
    delete[] dataFilesBuffer;
    delete[] dataFiles;
    free(indexFileName);
    free(dataFileName);
    if(compressedBuffers){
        delete [] compressedBuffers;
        delete [] compressedBufferSizes;
        delete [] cstream;
    }
}

void DBWriter::sortDatafileByIdOrder(DBReader<unsigned int> &dbr) {
    Debug(Debug::INFO) << "Sorting the results...  " << dataFileName << " .. ";

#pragma omp parallel
    {
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif

#pragma omp for schedule(static)
        for (size_t id = 0; id < dbr.getSize(); id++) {
            char *data = dbr.getData(id, thread_idx);
            size_t length = dbr.getSeqLens(id);
            writeData(data, (length == 0 ? 0 : length - 1), dbr.getDbKey(id), thread_idx);
        }
    };
    Debug(Debug::INFO) << "Done\n";
}

void DBWriter::mergeFiles(DBReader<unsigned int> &qdbr,
                          const std::vector<std::pair<std::string, std::string>>& files,
                          const std::vector<std::string>& prefixes) {
    Debug(Debug::INFO) << "Merging the results to " << dataFileName << "\n";

    // open DBReader
    const size_t fileCount = files.size();
    DBReader<unsigned int> **filesToMerge = new DBReader<unsigned int>*[fileCount];
    for (size_t i = 0; i < fileCount; i++) {
        filesToMerge[i] = new DBReader<unsigned int>(files[i].first.c_str(),
                                                     files[i].second.c_str(), 1, DBReader<unsigned int>::USE_DATA|DBReader<unsigned int>::USE_INDEX);
        filesToMerge[i]->open(DBReader<unsigned int>::NOSORT);
    }

    for (size_t id = 0; id < qdbr.getSize(); id++) {
        unsigned int key = qdbr.getDbKey(id);
        std::ostringstream ss;
        // get all data for the id from all files
        for (size_t i = 0; i < fileCount; i++) {
            const char *data = filesToMerge[i]->getDataByDBKey(key, 0);
            if (data != NULL) {
                if(i < prefixes.size()) {
                    ss << prefixes[i];
                }
                ss << data;
            }
        }
        // write result
        std::string result = ss.str();
        writeData(result.c_str(), result.length(), key, 0);
    }

    // close all reader
    for (size_t i = 0; i < fileCount; i++) {
        filesToMerge[i]->close();
        delete filesToMerge[i];
    }
    delete [] filesToMerge;

    Debug(Debug::INFO) << "Done\n";
}

// allocates heap memory, careful
char* makeResultFilename(const char* name, size_t split) {
    std::ostringstream ss;
    ss << name << "." << split;
    std::string s = ss.str();
    return strdup(s.c_str());
}

void DBWriter::open(size_t bufferSize) {
    for (unsigned int i = 0; i < threads; i++) {
        dataFileNames[i] = makeResultFilename(dataFileName, i);
        indexFileNames[i] = makeResultFilename(indexFileName, i);

        dataFiles[i] = fopen(dataFileNames[i], datafileMode.c_str());
        if (dataFiles[i] == NULL) {
            Debug(Debug::ERROR) << "Could not open " << dataFileNames[i] << " for writing!\n";
            EXIT(EXIT_FAILURE);
        }

        int fd = fileno(dataFiles[i]);
        int flags;
        if ((flags = fcntl(fd, F_GETFL, 0)) < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
            Debug(Debug::ERROR) << "Could not set mode for " << dataFileNames[i] << "!\n";
            EXIT(EXIT_FAILURE);
        }

        dataFilesBuffer[i] = new char[bufferSize];
        this->bufferSize = bufferSize;

        // set buffer to 64
        if (setvbuf(dataFiles[i], dataFilesBuffer[i], _IOFBF, bufferSize) != 0) {
            Debug(Debug::WARNING) << "Write buffer could not be allocated (bufferSize=" << bufferSize << ")\n";
        }

        indexFiles[i] = fopen(indexFileNames[i], "w");
        if (indexFiles[i] == NULL) {
            Debug(Debug::ERROR) << "Could not open " << indexFileNames[i] << " for writing!\n";
            EXIT(EXIT_FAILURE);
        }

        fd = fileno(indexFiles[i]);
        if ((flags = fcntl(fd, F_GETFL, 0)) < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
            Debug(Debug::ERROR) << "Could not set mode for " << indexFileNames[i] << "!\n";
            EXIT(EXIT_FAILURE);
        }

        if (setvbuf(indexFiles[i], NULL, _IOFBF, bufferSize) != 0) {
            Debug(Debug::WARNING) << "Write buffer could not be allocated (bufferSize=" << bufferSize << ")\n";
        }

        if (dataFiles[i] == NULL) {
            perror(dataFileNames[i]);
            EXIT(EXIT_FAILURE);
        }

        if (indexFiles[i] == NULL) {
            perror(indexFileNames[i]);
            EXIT(EXIT_FAILURE);
        }

        if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0){
            compressedBufferSizes[i] = 2097152;
            compressedBuffers[i] = (char*) malloc(compressedBufferSizes[i]);
            cstream[i] = ZSTD_createCStream();
        }
    }

    closed = false;
}

void DBWriter::writeDbtypeFile(const char* path, int dbtype, bool isCompressed) {
    std::string name = std::string(path) + ".dbtype";
    FILE* file = fopen(name.c_str(), "wb");
    if (file == NULL) {
        Debug(Debug::ERROR) << "Could not open data file " << name << "!\n";
        EXIT(EXIT_FAILURE);
    }
    dbtype = isCompressed ? dbtype | (1 << 31) : dbtype & ~(1 << 31);
    size_t written = fwrite(&dbtype, sizeof(int), 1, file);
    if (written != 1) {
        Debug(Debug::ERROR) << "Could not write to data file " << name << "\n";
        EXIT(EXIT_FAILURE);
    }
    fclose(file);
}


void DBWriter::close() {
    // close all datafiles
    for (unsigned int i = 0; i < threads; i++) {
        fclose(dataFiles[i]);
        fclose(indexFiles[i]);
    }

    if(compressedBuffers){
        for (unsigned int i = 0; i < threads; i++) {
            delete [] compressedBuffers[i];
            ZSTD_freeCStream(cstream[i]);
        }
    }

    writeDbtypeFile(dataFileName, dbtype, (mode & Parameters::WRITER_COMPRESSED_MODE) != 0);

    mergeResults(dataFileName, indexFileName,
                 (const char **) dataFileNames, (const char **) indexFileNames, threads, ((mode & Parameters::WRITER_LEXICOGRAPHIC_MODE) != 0));

    for (unsigned int i = 0; i < threads; i++) {
        delete [] dataFilesBuffer[i];
        free(dataFileNames[i]);
        free(indexFileNames[i]);
    }
    closed = true;
}

void DBWriter::writeStart(unsigned int thrIdx) {
    checkClosed();
    if (thrIdx >= threads) {
        Debug(Debug::ERROR) << "ERROR: Thread index " << thrIdx << " > maximum thread number " << threads << "\n";
        EXIT(EXIT_FAILURE);
    }

    starts[thrIdx] = offsets[thrIdx];

    if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0){
        int cLevel = 3;
        size_t const initResult = ZSTD_initCStream(cstream[thrIdx], cLevel);
        if (ZSTD_isError(initResult)) {
            Debug(Debug::ERROR) << "ERROR: ZSTD_initCStream() error in thread " << thrIdx << ". Error "
                                << ZSTD_getErrorName(initResult) << "\n";
            EXIT(EXIT_FAILURE);
        }
        // lets do some black magic! We need the first 4 byte for the length. We overwrite this later
        unsigned int dummyValue = 0;
        size_t written = fwrite(&dummyValue, sizeof(unsigned int), 1, dataFiles[thrIdx]);
        if(written != 1){
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[thrIdx] << "in writeStart\n";
            EXIT(EXIT_FAILURE);
        }
        offsets[thrIdx] += sizeof(unsigned int);
    }
}

size_t DBWriter::writeAdd(const char* data, size_t dataSize, unsigned int thrIdx) {
    checkClosed();
    if (thrIdx >= threads) {
        Debug(Debug::ERROR) << "ERROR: Thread index " << thrIdx << " > maximum thread number " << threads << "\n";
        EXIT(EXIT_FAILURE);
    }
    size_t totalWriten = 0;
    if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0) {
        ZSTD_inBuffer input = { data, dataSize, 0 };
        while (input.pos < input.size) {
            ZSTD_outBuffer output = {compressedBuffers[thrIdx], compressedBufferSizes[thrIdx], 0};
            size_t toRead = ZSTD_compressStream( cstream[thrIdx], &output, &input);   /* toRead is guaranteed to be <= ZSTD_CStreamInSize() */
            if (ZSTD_isError(toRead)) {
                Debug(Debug::ERROR) << "ERROR: ZSTD_compressStream() error in thread " << thrIdx << ". Error "
                                    << ZSTD_getErrorName(toRead) << "\n";
                EXIT(EXIT_FAILURE);
            }
            size_t written = fwrite(compressedBuffers[thrIdx], sizeof(char), output.pos, dataFiles[thrIdx]);
            if (written != output.pos) {
                Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[thrIdx] << "\n";
                EXIT(EXIT_FAILURE);
            }
            offsets[thrIdx] += written;
            totalWriten += written;
        }
    }else{
        size_t written = fwrite(data, sizeof(char), dataSize, dataFiles[thrIdx]);
        if (written != dataSize) {
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[thrIdx] << "\n";
            EXIT(EXIT_FAILURE);
        }
        offsets[thrIdx] += written;
    }

    return totalWriten;
}

void DBWriter::writeEnd(unsigned int key, unsigned int thrIdx, bool addNullByte, bool addIndexEntry) {
    // close stream
    if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0) {
        ZSTD_outBuffer output = {compressedBuffers[thrIdx], compressedBufferSizes[thrIdx], 0};
        const size_t remainingToFlush = ZSTD_endStream(cstream[thrIdx], &output); /* close frame */
        ZSTD_frameProgression progression = ZSTD_getFrameProgression(cstream[thrIdx]);
        size_t compressedLength = progression.produced;
//        std::cout << compressedLength << std::endl;
        if (remainingToFlush) {
            Debug(Debug::ERROR) << "Stream not flushed\n";
            EXIT(EXIT_FAILURE);
        }
        size_t written = fwrite(compressedBuffers[thrIdx], sizeof(char), output.pos, dataFiles[thrIdx]);
        offsets[thrIdx] += written;
        if (written != output.pos) {
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[thrIdx] << "\n";
            EXIT(EXIT_FAILURE);
        }
        // add compressed length to start
        if((mode & Parameters::WRITER_COMPRESSED_MODE) != 0) {
            unsigned int compressedLengthInt = static_cast<unsigned int>(compressedLength);
            int err = fseek(dataFiles[thrIdx], starts[thrIdx], SEEK_SET);
            if( err != 0 ){
                Debug(Debug::ERROR) << "Could not seek start in data file " << dataFileNames[thrIdx] << "\n";
                EXIT(EXIT_FAILURE);
            }
            size_t written2 = fwrite(&compressedLengthInt, sizeof(unsigned int), 1, dataFiles[thrIdx]);
            if (written2 != 1) {
                Debug(Debug::ERROR) << "Could not write entry length to data file " << dataFileNames[thrIdx] << "\n";
                EXIT(EXIT_FAILURE);
            }
            err = fseek(dataFiles[thrIdx], 0, SEEK_END);
            if( err != 0 ){
                Debug(Debug::ERROR) << "Could not seek end in data file " << dataFileNames[thrIdx] << "\n";
                EXIT(EXIT_FAILURE);
            }
        }
    }

    size_t totalWritten = 0;
    // entries are always separated by a null byte
    if (addNullByte == true) {
        char nullByte = '\0';
        const size_t written = fwrite(&nullByte, sizeof(char), 1, dataFiles[thrIdx]);
        if (written != 1) {
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[thrIdx] << "\n";
            EXIT(EXIT_FAILURE);
        }
        totalWritten += written;
        offsets[thrIdx] += 1;
    }

    if (addIndexEntry == true) {
        size_t length = offsets[thrIdx] - starts[thrIdx];
        // keep original size in index
        if ((mode & Parameters::WRITER_COMPRESSED_MODE) != 0) {
            ZSTD_frameProgression progression = ZSTD_getFrameProgression(cstream[thrIdx]);
            length = progression.consumed + totalWritten;
        }
        writeIndexEntry(key, starts[thrIdx], length, thrIdx);
    }
}

void DBWriter::writeIndexEntry(unsigned int key, size_t offset, size_t length, unsigned int thrIdx){
    char buffer[1024];
    size_t len = indexToBuffer(buffer, key, offset, length );
    size_t written = fwrite(buffer, sizeof(char), len, indexFiles[thrIdx]);
    if (written != len) {
        Debug(Debug::ERROR) << "Could not write to data file " << indexFiles[thrIdx] << "\n";
        EXIT(EXIT_FAILURE);
    }
}


void DBWriter::writeData(const char *data, size_t dataSize, unsigned int key, unsigned int thrIdx, bool addNullByte, bool addIndexEntry) {
    writeStart(thrIdx);
    writeAdd(data, dataSize, thrIdx);
    writeEnd(key, thrIdx, addNullByte, addIndexEntry);
}

size_t DBWriter::indexToBuffer(char *buff1, unsigned int key, size_t offsetStart, size_t len){
    char * basePos = buff1;
    char * tmpBuff = Itoa::u32toa_sse2(static_cast<uint32_t>(key), buff1);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::u64toa_sse2(static_cast<uint64_t>(offsetStart), tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::u64toa_sse2(static_cast<uint64_t>(len), tmpBuff);
    *(tmpBuff-1) = '\n';
    *(tmpBuff) = '\0';
    return tmpBuff - basePos;
}

void DBWriter::alignToPageSize() {
    if (threads > 1) {
        Debug(Debug::ERROR) << "Data file can only be aligned in single threaded mode.\n";
        EXIT(EXIT_FAILURE);
    }

    size_t currentOffset = offsets[0];
    size_t pageSize = Util::getPageSize();
    size_t newOffset = ((pageSize - 1) & currentOffset) ? ((currentOffset + pageSize) & ~(pageSize - 1)) : currentOffset;
    char nullByte = '\0';
    for (size_t i = currentOffset; i < newOffset; ++i) {
        size_t written = fwrite(&nullByte, sizeof(char), 1, dataFiles[0]);
        if (written != 1) {
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[0] << "\n";
            EXIT(EXIT_FAILURE);
        }
    }
    offsets[0] = newOffset;
}


void DBWriter::checkClosed() {
    if (closed == true) {
        Debug(Debug::ERROR) << "Trying to read a closed database. Datafile=" << dataFileName  << "\n";
        EXIT(EXIT_FAILURE);
    }
}

void DBWriter::mergeResults(const std::string &outFileName, const std::string &outFileNameIndex,
                            const std::vector<std::pair<std::string, std::string >> &files,
                            const  bool lexicographicOrder) {
    const char **datafilesNames = new const char *[files.size()];
    const char **indexFilesNames = new const char *[files.size()];
    for (size_t i = 0; i < files.size(); i++) {
        datafilesNames[i] = files[i].first.c_str();
        indexFilesNames[i] = files[i].second.c_str();
    }
    mergeResults(outFileName.c_str(), outFileNameIndex.c_str(), datafilesNames, indexFilesNames, files.size(), lexicographicOrder);
    delete[] datafilesNames;
    delete[] indexFilesNames;

    // leave only one dbtype file behind
    if (files.size() > 0) {
        std::string typeSrc = files[0].first + ".dbtype";
        std::string typeDest = outFileName + ".dbtype";
        if (FileUtil::fileExists(typeSrc.c_str())) {
            std::rename(typeSrc.c_str(), typeDest.c_str());
        }
        for (size_t i = 1; i < files.size(); i++) {
            std::string typeFile = files[i].first + ".dbtype";
            if (FileUtil::fileExists(typeFile.c_str())) {
                std::remove(typeFile.c_str());
            }
        }
    }
}

template <>
void DBWriter::writeIndex(FILE *outFile, size_t indexSize, DBReader<unsigned int>::Index *index,  unsigned int *seqLen){
    char buff1[1024];
    for (size_t id = 0; id < indexSize; id++) {
        char * tmpBuff = Itoa::u32toa_sse2((uint32_t)index[id].id,buff1);
        *(tmpBuff-1) = '\t';
        size_t currOffset = index[id].offset;
        tmpBuff = Itoa::u64toa_sse2(currOffset, tmpBuff);
        *(tmpBuff-1) = '\t';
        uint32_t sLen = seqLen[id];
        tmpBuff = Itoa::u32toa_sse2(sLen,tmpBuff);
        *(tmpBuff-1) = '\n';
        *(tmpBuff) = '\0';
        fwrite(buff1, sizeof(char), (tmpBuff - buff1), outFile);
    }
}

template <>
void DBWriter::writeIndex(FILE *outFile, size_t indexSize, DBReader<std::string>::Index *index,  unsigned int *seqLen){
    char buff1[1024];
    for (size_t id = 0; id < indexSize; id++) {
        size_t keyLen = index[id].id.length();
        char * tmpBuff = (char*)memcpy((void*)buff1, (void*)index[id].id.c_str(), keyLen);
        tmpBuff+=keyLen;
        *(tmpBuff) = '\t';
        tmpBuff++;
        size_t currOffset = index[id].offset;
        tmpBuff = Itoa::u64toa_sse2(currOffset, tmpBuff);
        *(tmpBuff-1) = '\t';
        uint32_t sLen = seqLen[id];
        tmpBuff = Itoa::u32toa_sse2(sLen,tmpBuff);
        *(tmpBuff-1) = '\n';
        *(tmpBuff) = '\0';
        fwrite(buff1, sizeof(char), (tmpBuff - buff1), outFile);
    }
}

void DBWriter::mergeResults(const char *outFileName, const char *outFileNameIndex,
                            const char **dataFileNames, const char **indexFileNames,
                            const unsigned long fileCount, const bool lexicographicOrder) {
    Timer timer;
    // merge results from each thread into one result file
    if (fileCount > 1) {
        FILE *outFile = fopen(outFileName, "w");
        FILE **infiles = new FILE *[fileCount];
        std::vector<size_t> threadDataFileSizes;

        for (unsigned int i = 0; i < fileCount; i++) {
            infiles[i] = fopen(dataFileNames[i], "r");
            if (infiles[i] == NULL) {
                Debug(Debug::ERROR) << "Could not open result file " << dataFileNames[i] << "!\n";
                EXIT(EXIT_FAILURE);
            }

            struct stat sb;
            if (fstat(fileno(infiles[i]), &sb) < 0) {
                int errsv = errno;
                Debug(Debug::ERROR) << "Failed to fstat file " << dataFileNames[i] << ". Error " << errsv << ".\n";
                EXIT(EXIT_FAILURE);
            }
            threadDataFileSizes.push_back(sb.st_size);
        }
        Concat::concatFiles(infiles, fileCount, outFile);
        for (unsigned int i = 0; i < fileCount; i++) {
            fclose(infiles[i]);
            if (std::remove(dataFileNames[i]) != 0) {
                Debug(Debug::WARNING) << "Could not remove file " << dataFileNames[i] << "\n";
            }
        }
        delete[] infiles;
        fclose(outFile);

        // merge index
        FILE *index_file = fopen(indexFileNames[0], "a");
        if (index_file == NULL) {
            perror(outFileNameIndex);
            EXIT(EXIT_FAILURE);
        }
        size_t globalOffset = threadDataFileSizes[0];
        for (unsigned int fileIdx = 1; fileIdx < fileCount; fileIdx++) {
            DBReader<unsigned int> reader(dataFileNames[fileIdx], indexFileNames[fileIdx], 1, DBReader<unsigned int>::USE_INDEX);
            reader.open(DBReader<unsigned int>::HARDNOSORT);
            if (reader.getSize() > 0) {
                DBReader<unsigned int>::Index * index = reader.getIndex();
                for (size_t i = 0; i < reader.getSize()-1; i++) {
                    size_t currOffset = index[i].offset;
                    index[i].offset = globalOffset + currOffset;
                }
                size_t currOffset = index[reader.getSize()-1].offset;
                index[reader.getSize()-1].offset = globalOffset + currOffset;
                writeIndex(index_file, reader.getSize(), index, reader.getSeqLens());
            }
            reader.close();
            if (std::remove(indexFileNames[fileIdx]) != 0) {
                Debug(Debug::WARNING) << "Could not remove file " << indexFileNames[fileIdx] << "\n";
            }

            globalOffset += threadDataFileSizes[fileIdx];
        }
        fclose(index_file);
    } else {
        if (std::rename(dataFileNames[0], outFileName) != 0) {
            Debug(Debug::ERROR) << "Could not move result " << dataFileNames[0] << " to final location " << outFileName << "!\n";
            EXIT(EXIT_FAILURE);
        }
    }

    if (lexicographicOrder == false) {
        // sort the index
        DBReader<unsigned int> indexReader(dataFileNames[0], indexFileNames[0], 1, DBReader<unsigned int>::USE_INDEX);
        indexReader.open(DBReader<unsigned int>::NOSORT);
        DBReader<unsigned int>::Index *index = indexReader.getIndex();
        FILE *index_file  = fopen(outFileNameIndex, "w");
        writeIndex(index_file, indexReader.getSize(), index, indexReader.getSeqLens());
        fclose(index_file);
        indexReader.close();

    } else {
        DBReader<std::string> indexReader(dataFileNames[0], indexFileNames[0], 1, DBReader<std::string>::USE_INDEX);
        indexReader.open(DBReader<std::string>::SORT_BY_ID);
        DBReader<std::string>::Index *index = indexReader.getIndex();
        FILE *index_file  = fopen(outFileNameIndex, "w");
        writeIndex(index_file, indexReader.getSize(), index, indexReader.getSeqLens());
        fclose(index_file);
        indexReader.close();
    }

    if (std::remove(indexFileNames[0]) != 0) {
        Debug(Debug::WARNING) << "Could not remove file " << indexFileNames[0] << "\n";
    }
    Debug(Debug::INFO) << "Time for merging files: " << timer.lap() << "\n";
}

void DBWriter::mergeFilePair(const std::vector<std::pair<std::string, std::string>> fileNames) {
    FILE ** files = new FILE*[fileNames.size()];
    for (size_t i = 0; i < fileNames.size();i++) {
        files[i] = FileUtil::openFileOrDie(fileNames[i].first.c_str(), "r", true);
#if HAVE_POSIX_FADVISE
        int status;
        if ((status = posix_fadvise (fileno(files[i]), 0, 0, POSIX_FADV_SEQUENTIAL)) != 0){
           Debug(Debug::ERROR) << "posix_fadvise returned an error: " << strerror(status) << "\n";
        }
#endif
    }

    int c1;
    char * buffer = dataFilesBuffer[0];
    size_t writePos = 0;
    int dataFilefd = fileno(dataFiles[0]);
    do {
        for (size_t i = 0; i < fileNames.size(); ++i) {
            while ((c1 = getc_unlocked(files[i])) != EOF) {
                if (c1 == '\0') {
                    break;
                }
                buffer[writePos] = (char) c1;
                writePos++;
                if (writePos == bufferSize) {
                    size_t written = write(dataFilefd, buffer, bufferSize);
                    if (written != bufferSize) {
                        Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[0] << "\n";
                        EXIT(EXIT_FAILURE);
                    }
                    writePos = 0;
                }
            }
        }
        buffer[writePos] = '\0';
        writePos++;
        if (writePos == bufferSize) {
            size_t written = write(dataFilefd, buffer, bufferSize);
            if (written != bufferSize) {
                Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[0] << "\n";
                EXIT(EXIT_FAILURE);
            }
            writePos = 0;
        }
    } while (c1!=EOF);

    if (writePos != 0) {
        // if there is data in the buffer that is not yet written
        size_t written = write(dataFilefd, (const void *) dataFilesBuffer[0], writePos);
        if (written != writePos) {
            Debug(Debug::ERROR) << "Could not write to data file " << dataFileNames[0] << "\n";
            EXIT(EXIT_FAILURE);
        }
    }

    for (size_t i = 0; i < fileNames.size(); ++i) {
        fclose(files[i]);
    }

    Debug(Debug::INFO) << "Merge file " << fileNames[0].first << " and " << fileNames[0].second << "\n";
    DBReader<unsigned int> reader1(fileNames[0].first.c_str(), fileNames[0].second.c_str(), 1,
                                   DBReader<unsigned int>::USE_INDEX);
    reader1.open(DBReader<unsigned int>::NOSORT);
    unsigned int *seqLen1 = reader1.getSeqLens();
    DBReader<unsigned int>::Index *index1 = reader1.getIndex();

    for (size_t i = 1; i < fileNames.size(); i++) {
        DBReader<unsigned int> reader2(fileNames[i].first.c_str(), fileNames[i].second.c_str(), 1,
                                       DBReader<unsigned int>::USE_INDEX);
        reader2.open(DBReader<unsigned int>::NOSORT);
        unsigned int *seqLen2 = reader2.getSeqLens();
        size_t currOffset = 0;

        for (size_t id = 0; id < reader1.getSize(); id++) {
            // add length for file1 and file2 and subtract -1 for one null byte
            size_t seqLen = seqLen1[id] + seqLen2[id] - 1;
            seqLen1[id] = seqLen;
            index1[id].offset = currOffset;
            currOffset += seqLen;
        }
        reader2.close();
    }

    writeIndex(indexFiles[0], reader1.getSize(), index1, seqLen1);
    reader1.close();
}
