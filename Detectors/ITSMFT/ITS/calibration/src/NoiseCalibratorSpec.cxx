// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// @file   NoiseCalibratorSpec.cxx

#include "CCDB/CcdbApi.h"
#include "DetectorsCalibration/Utils.h"
#include "ITSCalibration/NoiseCalibratorSpec.h"
#include "ITSCalibration/NoiseCalibrator.h"
#include "DataFormatsITSMFT/CompCluster.h"
#include "DataFormatsITSMFT/ROFRecord.h"

#include "FairLogger.h"
#include "Framework/ControlService.h"
#include "Framework/ConfigParamRegistry.h"

using namespace o2::framework;

namespace o2
{
namespace its
{

void NoiseCalibratorSpec::init(InitContext& ic)
{
  auto onepix = ic.options().get<bool>("1pix-only");
  LOG(INFO) << "Fast 1=pixel calibration: " << onepix;
  auto probT = ic.options().get<float>("prob-threshold");
  LOG(INFO) << "Setting the probability threshold to " << probT;

  mCalibrator = std::make_unique<o2::its::NoiseCalibrator>(onepix, probT);
}

void NoiseCalibratorSpec::run(ProcessingContext& pc)
{
  const auto compClusters = pc.inputs().get<gsl::span<o2::itsmft::CompClusterExt>>("compClusters");
  gsl::span<const unsigned char> patterns = pc.inputs().get<gsl::span<unsigned char>>("patterns");
  const auto rofs = pc.inputs().get<gsl::span<o2::itsmft::ROFRecord>>("ROframes");

  mCalibrator->processTimeFrame(compClusters, patterns, rofs);
}

void NoiseCalibratorSpec::endOfStream(o2::framework::EndOfStreamContext& ec)
{
  mCalibrator->finalize();
  const auto& payload = mCalibrator->getNoiseMap();

  std::map<std::string, std::string> md;
  o2::ccdb::CcdbObjectInfo info("ITS/Noise", "NoiseMap", "noise.root", md, 0, 9999999);

  auto image = o2::ccdb::CcdbApi::createObjectImage(&payload, &info);
  LOG(INFO) << "Sending object " << info.getPath() << "/" << info.getFileName()
            << " of size " << image->size()
            << " bytes, valid for " << info.getStartValidityTimestamp()
            << " : " << info.getEndValidityTimestamp();

  using clbUtils = o2::calibration::Utils;
  ec.outputs().snapshot(
    Output{clbUtils::gDataOriginCLB, clbUtils::gDataDescriptionCLBPayload, 0}, *image.get());
  ec.outputs().snapshot(
    Output{clbUtils::gDataOriginCLB, clbUtils::gDataDescriptionCLBInfo, 0}, info);
}

DataProcessorSpec getNoiseCalibratorSpec()
{
  std::vector<InputSpec> inputs;
  inputs.emplace_back("compClusters", "ITS", "COMPCLUSTERS", 0, Lifetime::Timeframe);
  inputs.emplace_back("patterns", "ITS", "PATTERNS", 0, Lifetime::Timeframe);
  inputs.emplace_back("ROframes", "ITS", "CLUSTERSROF", 0, Lifetime::Timeframe);

  using clbUtils = o2::calibration::Utils;
  std::vector<OutputSpec> outputs;
  outputs.emplace_back(
    ConcreteDataTypeMatcher{clbUtils::gDataOriginCLB, clbUtils::gDataDescriptionCLBPayload});
  outputs.emplace_back(
    ConcreteDataTypeMatcher{clbUtils::gDataOriginCLB, clbUtils::gDataDescriptionCLBInfo});

  return DataProcessorSpec{
    "its-noise-calibrator",
    inputs,
    outputs,
    AlgorithmSpec{adaptFromTask<NoiseCalibratorSpec>()},
    Options{
      {"1pix-only", VariantType::Bool, false, {"Fast 1-pixel calibration only"}},
      {"prob-threshold", VariantType::Float, 3.e-6f, {"Probability threshold for noisy pixels"}}}};
}

} // namespace its
} // namespace o2
