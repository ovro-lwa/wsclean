#include "partitionedms.h"

#include "averagingmsrowprovider.h"
#include "directmsrowprovider.h"
#include "msrowprovider.h"
#include "noisemsrowprovider.h"

#include "../system/system.h"

#include "../io/logger.h"

#include "../main/progressbar.h"
#include "../main/settings.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <memory>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <casacore/measures/Measures/MEpoch.h>

#include <casacore/measures/TableMeasures/ScalarMeasColumn.h>

// #define REDUNDANT_VALIDATION 1

PartitionedMS::PartitionedMS(const Handle& handle, size_t partIndex,
                             aocommon::PolarizationEnum polarization,
                             size_t dataDescId)
    : _handle(handle),
      _partIndex(partIndex),
      _metaFile(getMetaFilename(handle._data->_msPath,
                                handle._data->_temporaryDirectory, dataDescId)),
      _modelFileMap(0),
      _currentRow(0),
      _readPtrIsOk(true),
      _metaPtrIsOk(true),
      _weightPtrIsOk(true),
      _polarization(polarization),
      _polarizationCountInFile(
          _polarization == aocommon::Polarization::Instrumental ? 4 : 1) {
  _metaFile.read(reinterpret_cast<char*>(&_metaHeader), sizeof(MetaHeader));
  std::vector<char> msPath(_metaHeader.filenameLength + 1, char(0));
  _metaFile.read(msPath.data(), _metaHeader.filenameLength);
  Logger::Info << "Opening reordered part " << partIndex << " spw "
               << dataDescId << " for " << msPath.data() << '\n';
  std::string partPrefix =
      getPartPrefix(msPath.data(), partIndex, polarization, dataDescId,
                    handle._data->_temporaryDirectory);

  _dataFile.open(partPrefix + ".tmp", std::ios::in);
  if (!_dataFile.good())
    throw std::runtime_error("Error opening temporary data file '" +
                             partPrefix + ".tmp'");
  _dataFile.read(reinterpret_cast<char*>(&_partHeader), sizeof(PartHeader));
  if (!_dataFile.good())
    throw std::runtime_error("Error reading header from file '" + partPrefix +
                             ".tmp'");

  if (_partHeader.hasModel) {
    _fd = open((partPrefix + "-m.tmp").c_str(), O_RDWR);
    if (_fd == -1)
      throw std::runtime_error("Error opening temporary model data file '" +
                               partPrefix + "-m.tmp'");
    size_t length = _partHeader.channelCount * _metaHeader.selectedRowCount *
                    _polarizationCountInFile * sizeof(std::complex<float>);
    if (length == 0)
      _modelFileMap = nullptr;
    else {
      _modelFileMap =
          reinterpret_cast<char*>(mmap(NULL, length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_NORESERVE, _fd, 0));
      if (_modelFileMap == MAP_FAILED) {
        std::string msg = System::StrError(errno);
        _modelFileMap = 0;
        throw std::runtime_error(
            std::string("Error creating memory map to temporary model file: "
                        "mmap() returned MAP_FAILED with error message: ") +
            msg);
      }
    }
  }

  _weightFile.open(partPrefix + "-w.tmp", std::ios::in);
  if (!_weightFile.good())
    throw std::runtime_error("Error opening temporary data weight file '" +
                             partPrefix + "-w.tmp'");
  _weightBuffer.resize(_partHeader.channelCount * _polarizationCountInFile);
  _modelBuffer.resize(_partHeader.channelCount * _polarizationCountInFile);
}

PartitionedMS::~PartitionedMS() {
  if (_modelFileMap != 0) {
    size_t length = _partHeader.channelCount * _metaHeader.selectedRowCount *
                    _polarizationCountInFile * sizeof(std::complex<float>);
    if (length != 0) munmap(_modelFileMap, length);
  }
  if (_partHeader.hasModel) close(_fd);
}

void PartitionedMS::Reset() {
  _currentRow = 0;
  _metaFile.seekg(sizeof(MetaHeader) + _metaHeader.filenameLength,
                  std::ios::beg);
  _dataFile.seekg(sizeof(PartHeader), std::ios::beg);
  _weightFile.seekg(0, std::ios::beg);
  _readPtrIsOk = true;
  _metaPtrIsOk = true;
  _weightPtrIsOk = true;
}

bool PartitionedMS::CurrentRowAvailable() {
  return _currentRow < _metaHeader.selectedRowCount;
}

void PartitionedMS::NextRow() {
  ++_currentRow;
  if (_currentRow < _metaHeader.selectedRowCount) {
    if (_readPtrIsOk)
      _dataFile.seekg(_partHeader.channelCount * _polarizationCountInFile *
                          sizeof(std::complex<float>),
                      std::ios::cur);
    else
      _readPtrIsOk = true;

    if (_metaPtrIsOk)
      _metaFile.seekg(MetaRecord::BINARY_SIZE, std::ios::cur);
    else
      _metaPtrIsOk = true;

    if (_weightPtrIsOk)
      _weightFile.seekg(
          _partHeader.channelCount * _polarizationCountInFile * sizeof(float),
          std::ios::cur);
    _weightPtrIsOk = true;
  }
}

void PartitionedMS::ReadMeta(double& u, double& v, double& w,
                             size_t& dataDescId) {
  if (!_metaPtrIsOk) _metaFile.seekg(-MetaRecord::BINARY_SIZE, std::ios::cur);
  _metaPtrIsOk = false;

  MetaRecord record;
  record.read(_metaFile);
  u = record.u;
  v = record.v;
  w = record.w;
  dataDescId = record.dataDescId;
}

void PartitionedMS::ReadMeta(MetaData& metaData) {
  if (!_metaPtrIsOk) _metaFile.seekg(-MetaRecord::BINARY_SIZE, std::ios::cur);
  _metaPtrIsOk = false;

  MetaRecord record;
  record.read(_metaFile);
  metaData.uInM = record.u;
  metaData.vInM = record.v;
  metaData.wInM = record.w;
  metaData.dataDescId = record.dataDescId;
  metaData.fieldId = record.fieldId;
  metaData.antenna1 = record.antenna1;
  metaData.antenna2 = record.antenna2;
  metaData.time = record.time;
}

void PartitionedMS::ReadData(std::complex<float>* buffer) {
  if (!_readPtrIsOk) {
    _dataFile.seekg(-_partHeader.channelCount * _polarizationCountInFile *
                        sizeof(std::complex<float>),
                    std::ios::cur);
  }
#ifdef REDUNDANT_VALIDATION
  size_t pos = size_t(_dataFile.tellg()) - sizeof(PartHeader);
  size_t fact = _partHeader.hasModel ? 2 : 1;
  if (pos != fact * _currentRow * _partHeader.channelCount *
                 sizeof(std::complex<float>)) {
    std::ostringstream s;
    s << "Not on right pos: " << pos << " instead of "
      << fact * _currentRow * _partHeader.channelCount *
             sizeof(std::complex<float>)
      << " (row "
      << (pos / (fact * _partHeader.channelCount * sizeof(std::complex<float>)))
      << " instead of " << _currentRow << ")";
    throw std::runtime_error(s.str());
  }
#endif
  _dataFile.read(reinterpret_cast<char*>(buffer),
                 _partHeader.channelCount * _polarizationCountInFile *
                     sizeof(std::complex<float>));
  _readPtrIsOk = false;
}

void PartitionedMS::ReadModel(std::complex<float>* buffer) {
#ifdef REDUNDANT_VALIDATION
  if (!_partHeader.hasModel)
    throw std::runtime_error("Partitioned MS initialized without model");
#endif
  size_t rowLength = _partHeader.channelCount * _polarizationCountInFile *
                     sizeof(std::complex<float>);
  memcpy(reinterpret_cast<char*>(buffer),
         _modelFileMap + rowLength * _currentRow, rowLength);
}

void PartitionedMS::WriteModel(size_t rowId,
                               const std::complex<float>* buffer) {
#ifdef REDUNDANT_VALIDATION
  if (!_partHeader.hasModel)
    throw std::runtime_error("Partitioned MS initialized without model");
#endif
  size_t rowLength = _partHeader.channelCount * _polarizationCountInFile *
                     sizeof(std::complex<float>);
  std::complex<float>* modelWritePtr =
      reinterpret_cast<std::complex<float>*>(_modelFileMap + rowLength * rowId);

  // In case the value was not sampled in this pass, it has been set to infinite
  // and should not overwrite the current value in the set.
  for (size_t i = 0; i != _partHeader.channelCount * _polarizationCountInFile;
       ++i) {
    if (std::isfinite(buffer[i].real())) modelWritePtr[i] = buffer[i];
  }
}

void PartitionedMS::WriteImagingWeights(size_t rowId, const float* buffer) {
  if (_modelDataFile == nullptr) {
    std::string partPrefix = getPartPrefix(
        _handle._data->_msPath, _partIndex, _polarization,
        _partHeader.dataDescId, _handle._data->_temporaryDirectory);
    _imagingWeightsFile.reset(
        new std::fstream(partPrefix + "-imgw.tmp",
                         std::ios::in | std::ios::out | std::ios::binary));
  }
  const size_t chunkSize =
      _partHeader.channelCount * _polarizationCountInFile * sizeof(float);
  _imagingWeightsFile->seekg(chunkSize * rowId, std::ios::beg);
  _imagingWeightsFile->read(
      reinterpret_cast<char*>(_imagingWeightBuffer.data()),
      _partHeader.channelCount * _polarizationCountInFile * sizeof(float));
  for (size_t i = 0; i != _partHeader.channelCount * _polarizationCountInFile;
       ++i) {
    if (std::isfinite(buffer[i])) _imagingWeightBuffer[i] = buffer[i];
  }
  _imagingWeightsFile->seekp(chunkSize * rowId, std::ios::beg);
  _imagingWeightsFile->write(
      reinterpret_cast<const char*>(_imagingWeightBuffer.data()),
      _partHeader.channelCount * _polarizationCountInFile * sizeof(float));
}

void PartitionedMS::ReadWeights(std::complex<float>* buffer) {
  if (!_weightPtrIsOk)
    _weightFile.seekg(-_partHeader.channelCount * sizeof(float), std::ios::cur);
  float* displacedBuffer = reinterpret_cast<float*>(buffer) +
                           _partHeader.channelCount * _polarizationCountInFile;
  _weightFile.read(
      reinterpret_cast<char*>(displacedBuffer),
      _partHeader.channelCount * _polarizationCountInFile * sizeof(float));
  _weightPtrIsOk = false;
  copyRealToComplex(buffer, displacedBuffer,
                    _partHeader.channelCount * _polarizationCountInFile);
}

void PartitionedMS::ReadWeights(float* buffer) {
  if (!_weightPtrIsOk)
    _weightFile.seekg(
        -_partHeader.channelCount * _polarizationCountInFile * sizeof(float),
        std::ios::cur);
  _weightFile.read(
      reinterpret_cast<char*>(buffer),
      _partHeader.channelCount * _polarizationCountInFile * sizeof(float));
  _weightPtrIsOk = false;
}
std::string PartitionedMS::getFilenamePrefix(const std::string& msPathStr,
                                             const std::string& tempDir) {
  boost::filesystem::path prefixPath;
  if (tempDir.empty())
    prefixPath = msPathStr;
  else {
    std::string msPathCopy(msPathStr);
    while (!msPathCopy.empty() && *msPathCopy.rbegin() == '/')
      msPathCopy.resize(msPathCopy.size() - 1);
    boost::filesystem::path msPath(msPathCopy);
    prefixPath = boost::filesystem::path(tempDir) / msPath.filename();
  }
  std::string prefix(prefixPath.string());
  while (!prefix.empty() && *prefix.rbegin() == '/')
    prefix.resize(prefix.size() - 1);
  return prefix;
}

std::string PartitionedMS::getPartPrefix(const std::string& msPathStr,
                                         size_t partIndex,
                                         aocommon::PolarizationEnum pol,
                                         size_t dataDescId,
                                         const std::string& tempDir) {
  std::string prefix = getFilenamePrefix(msPathStr, tempDir);

  std::ostringstream partPrefix;
  partPrefix << prefix << "-part";
  if (partIndex < 1000) partPrefix << '0';
  if (partIndex < 100) partPrefix << '0';
  if (partIndex < 10) partPrefix << '0';
  partPrefix << partIndex;
  partPrefix << "-";
  partPrefix << aocommon::Polarization::TypeToShortString(pol);
  partPrefix << "-b" << dataDescId;
  return partPrefix.str();
}

string PartitionedMS::getMetaFilename(const string& msPathStr,
                                      const std::string& tempDir,
                                      size_t dataDescId) {
  std::string prefix = getFilenamePrefix(msPathStr, tempDir);

  std::ostringstream s;
  s << prefix << "-spw" << dataDescId << "-parted-meta.tmp";
  return s.str();
}

// should be private but is not allowed on older compilers
struct PartitionFiles {
  std::unique_ptr<std::ofstream> data, weight, model;
};

/*
 * When partitioned:
 * One global file stores:
 * - Metadata:
 *   * Number of selected rows
 *   * Filename length + string
 *   * [ UVW, dataDescId ]
 * The binary parts store the following information:
 * - Number of channels
 * - Start channel in MS
 * - Total weight in part
 * - Data    (single polarization, as requested)
 * - Weights (single)
 * - Model, optionally
 */
PartitionedMS::Handle PartitionedMS::Partition(
    const string& msPath, const std::vector<ChannelRange>& channels,
    MSSelection& selection, const string& dataColumnName, bool includeModel,
    bool initialModelRequired, const Settings& settings) {
  const bool modelUpdateRequired = settings.modelUpdateRequired;
  std::set<aocommon::PolarizationEnum> polsOut;
  if (settings.useIDG)
    polsOut.insert(aocommon::Polarization::Instrumental);
  else
    polsOut = settings.polarizations;
  const std::string& temporaryDirectory = settings.temporaryDirectory;

  size_t channelParts = channels.size();

  if (channelParts != 1) {
    Logger::Debug << "Partitioning in " << channels.size() << " channels:";
    for (size_t i = 0; i != channels.size(); ++i)
      Logger::Debug << ' ' << channels[i].dataDescId << ':' << channels[i].start
                    << '-' << channels[i].end;
  }
  Logger::Debug << '\n';

  // We need to enumerate the data desc ids, because each one needs a separate
  // meta file because they can have different uvws and other info
  std::map<size_t, size_t> selectedDataDescIds;
  getDataDescIdMap(selectedDataDescIds, channels);

  // Ordered as files[pol x channelpart]
  std::vector<PartitionFiles> files(channelParts * polsOut.size());

  size_t fileIndex = 0, maxChannels = 0;
  for (size_t part = 0; part != channelParts; ++part) {
    maxChannels =
        std::max(maxChannels, channels[part].end - channels[part].start);
    for (aocommon::PolarizationEnum p : polsOut) {
      PartitionFiles& f = files[fileIndex];
      std::string partPrefix = getPartPrefix(
          msPath, part, p, channels[part].dataDescId, temporaryDirectory);
      f.data.reset(new std::ofstream(partPrefix + ".tmp"));
      f.weight.reset(new std::ofstream(partPrefix + "-w.tmp"));
      if (initialModelRequired)
        f.model.reset(new std::ofstream(partPrefix + "-m.tmp"));
      f.data->seekp(sizeof(PartHeader), std::ios::beg);

      ++fileIndex;
    }
  }

  std::unique_ptr<MSRowProvider> rowProvider;
  if (settings.baselineDependentAveragingInWavelengths == 0.0) {
    if (settings.simulateNoise)
      rowProvider.reset(new NoiseMSRowProvider(
          settings.simulatedNoiseStdDev, msPath, selection, selectedDataDescIds,
          dataColumnName, initialModelRequired));
    else
      rowProvider.reset(
          new DirectMSRowProvider(msPath, selection, selectedDataDescIds,
                                  dataColumnName, initialModelRequired));
  } else {
    if (initialModelRequired)
      throw std::runtime_error(
          "Baseline-dependent averaging is enabled together with a model that "
          "requires the model data (e.g. -continue or -subtract-model). This "
          "is not possible.");
    rowProvider.reset(new AveragingMSRowProvider(
        settings.baselineDependentAveragingInWavelengths, msPath, selection,
        selectedDataDescIds, settings.fieldIds[0], dataColumnName,
        initialModelRequired));
  }

  std::vector<aocommon::PolarizationEnum> msPolarizations =
      GetMSPolarizations(rowProvider->MS());
  size_t nAntennas = rowProvider->MS().antenna().nrow();

  const casacore::IPosition shape(rowProvider->DataShape());

  if (settings.parallelReordering == 1)
    Logger::Info << "Reordering " << msPath << " into " << channelParts << " x "
                 << polsOut.size() << " parts.\n";

  // Write header of meta file, one meta file for each data desc id
  // TODO rather than writing we can just skip and write later
  std::vector<std::unique_ptr<std::ofstream>> metaFiles(
      selectedDataDescIds.size());
  for (std::map<size_t, size_t>::const_iterator i = selectedDataDescIds.begin();
       i != selectedDataDescIds.end(); ++i) {
    size_t dataDescId = i->first;
    size_t spwIndex = i->second;
    std::string metaFilename =
        getMetaFilename(msPath, temporaryDirectory, dataDescId);
    metaFiles[spwIndex].reset(new std::ofstream(metaFilename));
    MetaHeader metaHeader;
    memset(&metaHeader, 0, sizeof(MetaHeader));
    metaHeader.selectedRowCount = 0;  // not yet known
    metaHeader.filenameLength = msPath.size();
    metaHeader.startTime = rowProvider->StartTime();
    metaFiles[spwIndex]->write(reinterpret_cast<char*>(&metaHeader),
                               sizeof(metaHeader));
    metaFiles[spwIndex]->write(msPath.c_str(), msPath.size());
    if (!metaFiles[spwIndex]->good())
      throw std::runtime_error("Error writing to temporary file " +
                               metaFilename);
  }

  // Write actual data
  size_t polarizationsPerFile = settings.useIDG ? 4 : 1;
  std::vector<std::complex<float>> dataBuffer(polarizationsPerFile *
                                              maxChannels);
  std::vector<float> weightBuffer(polarizationsPerFile * maxChannels);

  casacore::Array<std::complex<float>> dataArray(shape), modelArray(shape);
  casacore::Array<float> weightSpectrumArray(shape);
  casacore::Array<bool> flagArray(shape);

  std::unique_ptr<ProgressBar> progress1;
  if (settings.parallelReordering == 1)
    progress1.reset(new ProgressBar("Reordering"));

  size_t selectedRowsTotal = 0;
  aocommon::UVector<size_t> selectedRowCountPerSpwIndex(
      selectedDataDescIds.size(), 0);
  while (!rowProvider->AtEnd()) {
    if (progress1)
      progress1->SetProgress(rowProvider->CurrentProgress(),
                             rowProvider->TotalProgress());

    MetaRecord meta;
    memset(&meta, 0, sizeof(MetaRecord));

    double time;
    uint32_t dataDescId, antenna1, antenna2, fieldId;
    rowProvider->ReadData(dataArray, flagArray, weightSpectrumArray, meta.u,
                          meta.v, meta.w, dataDescId, antenna1, antenna2,
                          fieldId, time);
    meta.dataDescId = dataDescId;
    meta.antenna1 = antenna1;
    meta.antenna2 = antenna2;
    meta.fieldId = fieldId;
    meta.time = time;
    size_t spwIndex = selectedDataDescIds[meta.dataDescId];
    ++selectedRowCountPerSpwIndex[spwIndex];
    ++selectedRowsTotal;
    std::ofstream& metaFile = *metaFiles[spwIndex];
    meta.write(metaFile);
    if (!metaFile.good())
      throw std::runtime_error("Error writing to temporary file");

    if (initialModelRequired) rowProvider->ReadModel(modelArray);

    fileIndex = 0;
    for (size_t part = 0; part != channelParts; ++part) {
      if (channels[part].dataDescId == int(meta.dataDescId)) {
        size_t partStartCh = channels[part].start,
               partEndCh = channels[part].end;

        for (aocommon::PolarizationEnum p : polsOut) {
          PartitionFiles& f = files[fileIndex];
          copyData(dataBuffer.data(), partStartCh, partEndCh, msPolarizations,
                   dataArray, p);
          f.data->write(reinterpret_cast<char*>(dataBuffer.data()),
                        (partEndCh - partStartCh) *
                            sizeof(std::complex<float>) * polarizationsPerFile);
          if (!f.data->good())
            throw std::runtime_error("Error writing to temporary data file");

          if (initialModelRequired) {
            copyData(dataBuffer.data(), partStartCh, partEndCh, msPolarizations,
                     modelArray, p);
            f.model->write(reinterpret_cast<char*>(dataBuffer.data()),
                           (partEndCh - partStartCh) *
                               sizeof(std::complex<float>) *
                               polarizationsPerFile);
            if (!f.model->good())
              throw std::runtime_error(
                  "Error writing to temporary model data file");
          }

          copyWeights(weightBuffer.data(), partStartCh, partEndCh,
                      msPolarizations, dataArray, weightSpectrumArray,
                      flagArray, p);
          f.weight->write(
              reinterpret_cast<char*>(weightBuffer.data()),
              (partEndCh - partStartCh) * sizeof(float) * polarizationsPerFile);
          if (!f.weight->good())
            throw std::runtime_error("Error writing to temporary weights file");
          ++fileIndex;
        }
      } else {
        fileIndex += polsOut.size();
      }
    }

    rowProvider->NextRow();
  }
  progress1.reset();
  Logger::Debug << "Total selected rows: " << selectedRowsTotal << '\n';
  rowProvider->OutputStatistics();

  // Rewrite meta headers to include selected row count
  for (std::map<size_t, size_t>::const_iterator i = selectedDataDescIds.begin();
       i != selectedDataDescIds.end(); ++i) {
    size_t spwIndex = i->second;
    MetaHeader metaHeader;
    memset(&metaHeader, 0, sizeof(MetaHeader));
    metaHeader.selectedRowCount = selectedRowCountPerSpwIndex[spwIndex];
    metaHeader.filenameLength = msPath.size();
    metaHeader.startTime = rowProvider->StartTime();
    metaFiles[spwIndex]->seekp(0);
    metaFiles[spwIndex]->write(reinterpret_cast<char*>(&metaHeader),
                               sizeof(metaHeader));
    metaFiles[spwIndex]->write(msPath.c_str(), msPath.size());
  }

  // Write header to parts and write empty model files (if requested)
  PartHeader header;
  memset(&header, 0, sizeof(PartHeader));
  header.hasModel = includeModel;
  fileIndex = 0;
  dataBuffer.assign(maxChannels * polarizationsPerFile, 0.0);
  std::unique_ptr<ProgressBar> progress2;
  if (includeModel && !initialModelRequired && settings.parallelReordering == 1)
    progress2.reset(new ProgressBar("Initializing model visibilities"));
  for (size_t part = 0; part != channelParts; ++part) {
    header.channelStart = channels[part].start,
    header.channelCount = channels[part].end - header.channelStart;
    header.dataDescId = channels[part].dataDescId;
    for (std::set<aocommon::PolarizationEnum>::const_iterator p =
             polsOut.begin();
         p != polsOut.end(); ++p) {
      PartitionFiles& f = files[fileIndex];
      f.data->seekp(0, std::ios::beg);
      f.data->write(reinterpret_cast<char*>(&header), sizeof(PartHeader));
      if (!f.data->good())
        throw std::runtime_error("Error writing to temporary data file");

      f.data.reset();
      f.weight.reset();
      f.model.reset();
      ++fileIndex;

      // If model is requested, fill model file with zeros
      if (includeModel && !initialModelRequired) {
        std::string partPrefix = getPartPrefix(
            msPath, part, *p, header.dataDescId, temporaryDirectory);
        std::ofstream modelFile(partPrefix + "-m.tmp");
        const size_t selectedRowCount = selectedRowCountPerSpwIndex
            [selectedDataDescIds[channels[part].dataDescId]];
        for (size_t i = 0; i != selectedRowCount; ++i) {
          modelFile.write(reinterpret_cast<char*>(dataBuffer.data()),
                          header.channelCount * sizeof(std::complex<float>) *
                              polarizationsPerFile);
          if (progress2)
            progress2->SetProgress(part * selectedRowCount + i,
                                   channelParts * selectedRowCount);
        }
      }
    }
  }
  progress2.reset();

  return Handle(msPath, dataColumnName, temporaryDirectory, channels,
                initialModelRequired, modelUpdateRequired, polsOut, selection,
                nAntennas);
}

void PartitionedMS::unpartition(
    const PartitionedMS::Handle::HandleData& handle) {
  const std::set<aocommon::PolarizationEnum> pols = handle._polarizations;

  std::map<size_t, size_t> dataDescIds;
  getDataDescIdMap(dataDescIds, handle._channels);

  std::vector<MetaHeader> metaHeaders(dataDescIds.size());
  for (const std::pair<size_t, size_t>& dataDescId : dataDescIds) {
    std::ifstream metaFile(getMetaFilename(
        handle._msPath, handle._temporaryDirectory, dataDescId.first));
    MetaHeader& metaHeader = metaHeaders[dataDescId.second];
    metaFile.read(reinterpret_cast<char*>(&metaHeader), sizeof(MetaHeader));
    std::vector<char> msPath(metaHeader.filenameLength + 1, char(0));
    metaFile.read(msPath.data(), metaHeader.filenameLength);
  }

  ChannelRange firstRange = handle._channels[0];
  std::ifstream firstDataFile(
      getPartPrefix(handle._msPath, 0, *pols.begin(), firstRange.dataDescId,
                    handle._temporaryDirectory) +
          ".tmp",
      std::ios::in);
  if (!firstDataFile.good())
    throw std::runtime_error("Error opening temporary data file");
  PartHeader firstPartHeader;
  firstDataFile.read(reinterpret_cast<char*>(&firstPartHeader),
                     sizeof(PartHeader));
  if (!firstDataFile.good())
    throw std::runtime_error("Error reading from temporary data file");

  if (firstPartHeader.hasModel) {
    const size_t channelParts = handle._channels.size();

    // Open the temporary files
    std::vector<std::unique_ptr<std::ifstream>> modelFiles(channelParts *
                                                           pols.size());
    size_t fileIndex = 0;
    for (size_t part = 0; part != channelParts; ++part) {
      size_t dataDescId = handle._channels[part].dataDescId;
      for (std::set<aocommon::PolarizationEnum>::const_iterator p =
               pols.begin();
           p != pols.end(); ++p) {
        std::string partPrefix = getPartPrefix(
            handle._msPath, part, *p, dataDescId, handle._temporaryDirectory);
        modelFiles[fileIndex].reset(new std::ifstream(partPrefix + "-m.tmp"));
        ++fileIndex;
      }
    }

    casacore::MeasurementSet ms(handle._msPath, casacore::Table::Update);
    const std::vector<aocommon::PolarizationEnum> msPolarizations =
        GetMSPolarizations(ms);
    initializeModelColumn(ms);
    casacore::ScalarColumn<int> antenna1Column(
        ms, ms.columnName(casacore::MSMainEnums::ANTENNA1));
    casacore::ScalarColumn<int> antenna2Column(
        ms, ms.columnName(casacore::MSMainEnums::ANTENNA2));
    casacore::ScalarColumn<int> fieldIdColumn(
        ms, ms.columnName(casacore::MSMainEnums::FIELD_ID));
    casacore::ScalarColumn<double> timeColumn(
        ms, ms.columnName(casacore::MSMainEnums::TIME));
    casacore::ScalarColumn<int> dataDescIdColumn(
        ms, ms.columnName(casacore::MSMainEnums::DATA_DESC_ID));
    casacore::ArrayColumn<casacore::Complex> dataColumn(ms,
                                                        handle._dataColumnName);
    casacore::ArrayColumn<casacore::Complex> modelColumn(
        ms, ms.columnName(casacore::MSMainEnums::MODEL_DATA));
    casacore::ArrayColumn<double> uvwColumn(
        ms, ms.columnName(casacore::MSMainEnums::UVW));

    const casacore::IPosition shape(dataColumn.shape(0));
    size_t channelCount = shape[1];

    size_t polarizationsPerFile =
        (*pols.begin()) == aocommon::Polarization::Instrumental ? 4 : 1;
    std::vector<std::complex<float>> modelDataBuffer(channelCount *
                                                     polarizationsPerFile);
    std::vector<float> weightBuffer(channelCount * polarizationsPerFile);
    casacore::Array<std::complex<float>> modelDataArray(shape);

    ProgressBar progress(std::string("Writing changed model back to ") +
                         handle._msPath);
    size_t startRow, endRow;
    getRowRange(ms, handle._selection, startRow, endRow);
    size_t timestep =
        handle._selection.HasInterval() ? handle._selection.IntervalStart() : 0;
    double time = timeColumn(startRow);
    size_t selectedRowCountForDebug = 0;
    for (size_t row = startRow; row != endRow; ++row) {
      progress.SetProgress(row - startRow, endRow - startRow);
      const int a1 = antenna1Column(row), a2 = antenna2Column(row),
                fieldId = fieldIdColumn(row),
                dataDescId = dataDescIdColumn(row);
      casacore::Vector<double> uvw = uvwColumn(row);

      if (time != timeColumn(row)) {
        ++timestep;
        time = timeColumn(row);
      }
      if (handle._selection.IsSelected(fieldId, timestep, a1, a2, uvw)) {
        std::map<size_t, size_t>::const_iterator dataDescIdIter =
            dataDescIds.find(dataDescId);
        if (dataDescIdIter != dataDescIds.end()) {
          modelColumn.get(row, modelDataArray);
          size_t fileIndex = 0;
          for (size_t part = 0; part != channelParts; ++part) {
            size_t dataDescId = handle._channels[part].dataDescId,
                   partStartCh = handle._channels[part].start,
                   partEndCh = handle._channels[part].end;
            if (dataDescId == dataDescIdIter->second) {
              for (std::set<aocommon::PolarizationEnum>::const_iterator p =
                       pols.begin();
                   p != pols.end(); ++p) {
                modelFiles[fileIndex]->read(
                    reinterpret_cast<char*>(modelDataBuffer.data()),
                    (partEndCh - partStartCh) * polarizationsPerFile *
                        sizeof(std::complex<float>));
                if (!modelFiles[fileIndex]->good())
                  throw std::runtime_error(
                      "Error reading from temporary model data file");
                reverseCopyData(modelDataArray, partStartCh, partEndCh,
                                msPolarizations, modelDataBuffer.data(), *p);

                ++fileIndex;
              }
            } else {
              fileIndex += pols.size();
            }
          }
          modelColumn.put(row, modelDataArray);
          selectedRowCountForDebug++;
        }
      }
    }
    progress.SetProgress(ms.nrow(), ms.nrow());

    Logger::Debug << "Row count during unpartitioning: "
                  << selectedRowCountForDebug << '\n';
  }
}

PartitionedMS::Handle::HandleData::~HandleData() {
  if (!_isCopy) {
    if (_modelUpdateRequired) PartitionedMS::unpartition(*this);

    Logger::Info << "Cleaning up temporary files...\n";

    std::set<size_t> removedMetaFiles;
    for (size_t part = 0; part != _channels.size(); ++part) {
      for (aocommon::PolarizationEnum p : _polarizations) {
        std::string prefix = getPartPrefix(
            _msPath, part, p, _channels[part].dataDescId, _temporaryDirectory);
        std::remove((prefix + ".tmp").c_str());
        std::remove((prefix + "-w.tmp").c_str());
        std::remove((prefix + "-m.tmp").c_str());
      }
      size_t dataDescId = _channels[part].dataDescId;
      if (removedMetaFiles.count(dataDescId) == 0) {
        removedMetaFiles.insert(dataDescId);
        std::string metaFile =
            getMetaFilename(_msPath, _temporaryDirectory, dataDescId);
        std::remove(metaFile.c_str());
      }
    }
  }
}

void PartitionedMS::MakeIdToMSRowMapping(std::vector<size_t>& idToMSRow) {
  const MSSelection& selection = _handle._data->_selection;
  std::map<size_t, size_t> dataDescIds;
  getDataDescIdMap(dataDescIds, _handle._data->_channels);
  std::set<size_t> dataDescIdSet;
  for (std::map<size_t, size_t>::const_iterator i = dataDescIds.begin();
       i != dataDescIds.end(); ++i)
    dataDescIdSet.insert(i->first);
  size_t startRow, endRow;
  SynchronizedMS ms = MS();
  getRowRangeAndIDMap(*ms, selection, startRow, endRow, dataDescIdSet,
                      idToMSRow);
}

void PartitionedMS::getDataDescIdMap(
    std::map<size_t, size_t>& dataDescIds,
    const std::vector<PartitionedMS::ChannelRange>& channels) {
  size_t spwIndex = 0;
  for (size_t i = 0; i != channels.size(); ++i) {
    if (dataDescIds.count(channels[i].dataDescId) == 0) {
      dataDescIds.insert(std::make_pair(channels[i].dataDescId, spwIndex));
      ++spwIndex;
    }
  }
}

void PartitionedMS::Handle::Serialize(aocommon::SerialOStream& stream) const {
  stream.Ptr(_data);
}

void PartitionedMS::Handle::Unserialize(aocommon::SerialIStream& stream) {
  stream.Ptr(_data);
}

void PartitionedMS::Handle::HandleData::Serialize(
    aocommon::SerialOStream& stream) const {
  stream.String(_msPath)
      .String(_dataColumnName)
      .String(_temporaryDirectory)
      .UInt64(_channels.size());
  for (const ChannelRange& range : _channels) {
    stream.UInt64(range.dataDescId).UInt64(range.start).UInt64(range.end);
  }
  stream.Bool(_initialModelRequired)
      .Bool(_modelUpdateRequired)
      .UInt64(_polarizations.size());
  for (aocommon::PolarizationEnum p : _polarizations) stream.UInt32(p);
  _selection.Serialize(stream);
  stream.UInt64(_nAntennas);
}

void PartitionedMS::Handle::HandleData::Unserialize(
    aocommon::SerialIStream& stream) {
  _isCopy = true;
  stream.String(_msPath).String(_dataColumnName).String(_temporaryDirectory);
  _channels.resize(stream.UInt64());
  for (ChannelRange& range : _channels) {
    stream.UInt64(range.dataDescId).UInt64(range.start).UInt64(range.end);
  }
  stream.Bool(_initialModelRequired).Bool(_modelUpdateRequired);
  size_t nPol = stream.UInt64();
  _polarizations.clear();
  for (size_t i = 0; i != nPol; ++i)
    _polarizations.emplace((aocommon::PolarizationEnum)stream.UInt32());
  _selection.Unserialize(stream);
  stream.UInt64(_nAntennas);
}
