/// \file DigitRow.cxx
/// \author Andi Mathis, andreas.mathis@ph.tum.de

#include "TPCSimulation/DigitRow.h"
#include "TPCSimulation/DigitPad.h"

using namespace AliceO2::TPC;

DigitRow::DigitRow(Int_t row, Int_t npads)
  : mRow(row),
    mPads(npads),
    mTotalChargeRow(0.)
{}

DigitRow::~DigitRow()
{
  for(auto &aPad : mPads) {
    if(aPad == nullptr) continue;
    delete aPad;
  }
}

void DigitRow::setDigit(Int_t pad, Float_t charge)
{
  DigitPad *result = mPads[pad];
  if(result != nullptr) {
    mPads[pad]->setDigit(charge);
  }
  else{
    mPads[pad] = new DigitPad(pad);
    mPads[pad]->setDigit(charge);
  }
}


void DigitRow::fillOutputContainer(TClonesArray *output, Int_t cru, Int_t timeBin, Int_t row)
{
  for(auto &aPad : mPads) {
    if(aPad == nullptr) continue;
    aPad->fillOutputContainer(output, cru, timeBin, row, aPad->getPad());
  }
}

void DigitRow::fillOutputContainer(TClonesArray *output, Int_t cru, Int_t timeBin, Int_t row, Float_t commonMode)
{
  for(auto &aPad : mPads) {
    if(aPad == nullptr) continue;
    aPad->fillOutputContainer(output, cru, timeBin, row, aPad->getPad(), commonMode);
  }
}

void DigitRow::processCommonMode(Int_t cru, Int_t timeBin, Int_t row)
{
  for(auto &aPad : mPads) {
    if(aPad == nullptr) continue;
    aPad->processCommonMode(cru, timeBin, row, aPad->getPad());
    mTotalChargeRow += aPad->getTotalChargePad();
  }
}
