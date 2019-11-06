// Copyright (c) 2011-2016 The Cryptonote developers, The Bytecoin developers
// Copyright (c) 2018, The BBSCoin Developers
// Copyright (c) 2018, The Karbo Developers
// Copyright (c) 2018-2019, The TurtleCoin Developers
// Copyright (c) 2018-2019 The Cash2 developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <cassert>
#include <ctime>
#include <numeric>
#include <random>
#include <set>
#include <tuple>
#include <utility>

#include "Common/ScopeExit.h"
#include "Common/ShuffleGenerator.h"
#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "ITransaction.h"
#include "System/EventLock.h"
#include "System/RemoteContext.h"
#include "Transfers/TransfersContainer.h"
#include "WalletErrors.h"
#include "WalletGreen.h"
#include "WalletSerialization.h"
#include "WalletUtils.h"

namespace {

void asyncRequestCompletion(System::Event& requestFinished)
{
  requestFinished.set();
}

void checkIfEnoughMixins(std::vector<CryptoNote::CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& mixinResult, uint64_t mixIn) {
  auto notEnoughIt = std::find_if(mixinResult.begin(), mixinResult.end(),
    [mixIn] (const CryptoNote::CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& ofa) { return ofa.outs.size() < mixIn; } );

  if (mixIn == 0 && mixinResult.empty()) {
    throw std::system_error(make_error_code(CryptoNote::error::MIXIN_COUNT_TOO_BIG));
  }

  if (notEnoughIt != mixinResult.end()) {
    throw std::system_error(make_error_code(CryptoNote::error::MIXIN_COUNT_TOO_BIG));
  }
}

CryptoNote::WalletEvent makeTransactionUpdatedEvent(size_t transactionIndex)
{
  CryptoNote::WalletEvent event;
  event.type = CryptoNote::WalletEventType::TRANSACTION_UPDATED;
  event.transactionUpdated.transactionIndex = transactionIndex;

  return event;
}

std::vector<CryptoNote::WalletTransfer> convertOrdersToTransfers(const std::vector<CryptoNote::WalletOrder>& walletOrders)
{
  std::vector<CryptoNote::WalletTransfer> walletTransfers;
  walletTransfers.reserve(walletOrders.size());

  for (const CryptoNote::WalletOrder& walletOrder: walletOrders)
  {
    CryptoNote::WalletTransfer walletTransfer;

    if (walletOrder.amount > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
      throw std::system_error(make_error_code(CryptoNote::error::WRONG_AMOUNT),
        "Order amount must not exceed " + std::to_string(std::numeric_limits<int64_t>::max()));
    }

    walletTransfer.type = CryptoNote::WalletTransferType::USUAL;
    walletTransfer.address = walletOrder.address;
    walletTransfer.amount = static_cast<int64_t>(walletOrder.amount);

    walletTransfers.emplace_back(std::move(walletTransfer));
  }

  return walletTransfers;
}

uint64_t calculateDonationAmount(uint64_t freeAmount, uint64_t donationThreshold, uint64_t dustThreshold) {
  std::vector<uint64_t> decomposedAmounts;
  CryptoNote::decomposeAmount(freeAmount, dustThreshold, decomposedAmounts);

  std::sort(decomposedAmounts.begin(), decomposedAmounts.end(), std::greater<uint64_t>());

  uint64_t donationAmount = 0;
  for (uint64_t amount: decomposedAmounts) {
    if (amount <= donationThreshold - donationAmount) {
      donationAmount += amount;
    }
  }

  assert(donationAmount <= freeAmount);

  return donationAmount;
}

uint64_t pushDonationTransferIfPossible(const CryptoNote::DonationSettings& donation, uint64_t freeAmount, uint64_t dustThreshold, std::vector<CryptoNote::WalletTransfer>& destinations) {
  uint64_t donationAmount = 0;
  if (!donation.address.empty() && donation.threshold != 0) {
    if (donation.threshold > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw std::system_error(make_error_code(CryptoNote::error::WRONG_AMOUNT),
        "Donation threshold must not exceed " + std::to_string(std::numeric_limits<int64_t>::max()));
    }

    donationAmount = calculateDonationAmount(freeAmount, donation.threshold, dustThreshold);
    if (donationAmount != 0) {
      destinations.emplace_back(CryptoNote::WalletTransfer {CryptoNote::WalletTransferType::DONATION, donation.address, static_cast<int64_t>(donationAmount)});
    }
  }

  return donationAmount;
}

CryptoNote::AccountPublicAddress parseAccountAddressString(const std::string& addressString, const CryptoNote::Currency& currency)
{
  CryptoNote::AccountPublicAddress address;

  if (!currency.parseAccountAddressString(addressString, address)) {
    throw std::system_error(make_error_code(CryptoNote::error::BAD_ADDRESS));
  }

  return address;
}

}

namespace CryptoNote {


// Public functions


WalletGreen::WalletGreen(System::Dispatcher& dispatcher, const Currency& currency, INode& node, uint32_t transactionSoftLockTime) :
  m_dispatcher(dispatcher),
  m_currency(currency),
  m_node(node),
  m_stopped(false),
  m_blockchainSynchronizerStarted(false),
  m_blockchainSynchronizer(node, currency.genesisBlockHash()),
  m_transfersSynchronizer(currency, m_blockchainSynchronizer, node),
  m_eventOccurred(m_dispatcher),
  m_readyEvent(m_dispatcher),
  m_walletState(WalletState::NOT_INITIALIZED),
  m_actualBalance(0),
  m_pendingBalance(0),
  m_transactionSoftLockTime(transactionSoftLockTime),
  m_refresh_progress_reporter(*this)
{
  m_upperTransactionSizeLimit = m_currency.blockGrantedFullRewardZone() * 2 - m_currency.minerTxBlobReservedSize();
  m_readyEvent.set();
}

WalletGreen::~WalletGreen() {
  if (m_walletState == WalletState::INITIALIZED) {
    doShutdown();
  }

  m_dispatcher.yield(); //let remote spawns finish
}

void WalletGreen::changePassword(const std::string& oldPassword, const std::string& newPassword) {
  throwIfNotInitialized();
  throwIfStopped();

  if (m_password.compare(oldPassword)) {
    throw std::system_error(make_error_code(error::WRONG_PASSWORD));
  }

  m_password = newPassword;
}

void WalletGreen::commitTransaction(size_t transactionIndex) {

  System::EventLock lk(m_readyEvent);

  throwIfNotInitialized();
  throwIfStopped();
  throwIfTrackingMode();

  if (transactionIndex >= m_walletTransactions.size())
  {
    throw std::system_error(make_error_code(error::INDEX_OUT_OF_RANGE));
  }

  auto txIt = std::next(m_walletTransactions.get<RandomAccessIndex>().begin(), transactionIndex);
  if (m_uncommitedTransactions.count(transactionIndex) == 0 || txIt->state != WalletTransactionState::CREATED) {
    throw std::system_error(make_error_code(error::TX_TRANSFER_IMPOSSIBLE));
  }

  System::Event completion(m_dispatcher);
  std::error_code ec;

  m_node.relayTransaction(m_uncommitedTransactions[transactionIndex], [&ec, &completion, this](std::error_code error) {
    ec = error;
    this->m_dispatcher.remoteSpawn(std::bind(asyncRequestCompletion, std::ref(completion)));
  });

  completion.wait();

  if (!ec) {
    updateTransactionStateAndPushEvent(transactionIndex, WalletTransactionState::SUCCEEDED);
    m_uncommitedTransactions.erase(transactionIndex);
  } else {
    throw std::system_error(ec);
  }
}

std::string WalletGreen::createAddress() {
  KeyPair spendKeyPair;
  Crypto::generate_keys(spendKeyPair.publicKey, spendKeyPair.secretKey);
  uint64_t creationTimestamp = static_cast<uint64_t>(time(nullptr));

  return doCreateAddress(spendKeyPair.publicKey, spendKeyPair.secretKey, creationTimestamp);
}

std::string WalletGreen::createAddress(const Crypto::SecretKey& spendPrivateKey) {
  Crypto::PublicKey spendPublicKey;
  if (!Crypto::secret_key_to_public_key(spendPrivateKey, spendPublicKey) ) {
    throw std::system_error(make_error_code(error::KEY_GENERATION_ERROR));
  }

  return doCreateAddress(spendPublicKey, spendPrivateKey, 0);
}

std::string WalletGreen::createAddress(const Crypto::PublicKey& spendPublicKey) {
  if (!Crypto::check_key(spendPublicKey)) {
    throw std::system_error(make_error_code(error::WRONG_PARAMETERS), "Wrong public key format");
  }

  return doCreateAddress(spendPublicKey, NULL_SECRET_KEY, 0);
}

// Plan to remove
size_t WalletGreen::createFusionTransaction(uint64_t threshold, uint64_t mixin) {
  Tools::ScopeExit releaseContext([this] {
    m_dispatcher.yield();
  });

  System::EventLock lk(m_readyEvent);

  throwIfNotInitialized();
  throwIfTrackingMode();
  throwIfStopped();

  const size_t MAX_FUSION_OUTPUT_COUNT = 4;

  uint64_t dustThreshold = m_currency.getDustThreshold(m_node.getLastKnownBlockHeight());

  if (threshold <= dustThreshold) {
    throw std::runtime_error("Threshold must be greater than " + std::to_string(dustThreshold));
  }

  if (m_walletsContainer.get<RandomAccessIndex>().size() == 0) {
    throw std::runtime_error("You must have at least one address");
  }

  size_t estimatedFusionInputsCount = m_currency.getApproximateMaximumInputCount(m_currency.fusionTxMaxSize(), MAX_FUSION_OUTPUT_COUNT, mixin);
  if (estimatedFusionInputsCount < m_currency.fusionTxMinInputCount()) {
    throw std::system_error(make_error_code(error::MIXIN_COUNT_TOO_BIG));
  }

  std::vector<OutputToTransfer> fusionInputs = pickRandomFusionInputs(threshold, m_currency.fusionTxMinInputCount(), estimatedFusionInputsCount);
  if (fusionInputs.size() < m_currency.fusionTxMinInputCount()) {
    //nothing to optimize
    return WALLET_INVALID_TRANSACTION_ID;
  }

  typedef CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount outs_for_amount;
  std::vector<outs_for_amount> mixinResult;
  if (mixin != 0) {
    requestMixinOuts(fusionInputs, mixin, mixinResult);
  }

  std::vector<InputInfo> keysInfo;
  prepareInputs(fusionInputs, mixinResult, mixin, keysInfo);

  std::unique_ptr<ITransaction> fusionTransaction;
  size_t transactionSize;
  int round = 0;
  uint64_t transactionAmount;
  do {
    if (round != 0) {
      fusionInputs.pop_back();
      keysInfo.pop_back();
    }

    uint64_t inputsAmount = std::accumulate(fusionInputs.begin(), fusionInputs.end(), static_cast<uint64_t>(0), [] (uint64_t amount, const OutputToTransfer& input) {
      return amount + input.out.amount;
    });

    transactionAmount = inputsAmount;

    ReceiverAmounts decomposedOutputs = decomposeFusionOutputs(inputsAmount);
    assert(decomposedOutputs.amounts.size() <= MAX_FUSION_OUTPUT_COUNT);

    Crypto::SecretKey txkey;
    fusionTransaction = makeTransaction(std::vector<ReceiverAmounts>{decomposedOutputs}, keysInfo, "", 0, txkey);

    transactionSize = fusionTransaction->getTransactionData().size();;

    ++round;
  } while (transactionSize > m_currency.fusionTxMaxSize() && fusionInputs.size() >= m_currency.fusionTxMinInputCount());

  if (fusionInputs.size() < m_currency.fusionTxMinInputCount()) {
    throw std::runtime_error("Unable to create fusion transaction");
  }

  return validateSaveAndSendTransaction(*fusionTransaction, {}, true, true);
}

void WalletGreen::deleteAddress(const std::string& address) {
  throwIfNotInitialized();
  throwIfStopped();

  AccountPublicAddress pubAddr = parseAddress(address);

  auto it = m_walletsContainer.get<KeysIndex>().find(pubAddr.spendPublicKey);
  if (it == m_walletsContainer.get<KeysIndex>().end()) {
    throw std::system_error(make_error_code(error::OBJECT_NOT_FOUND));
  }

  stopBlockchainSynchronizer();

  m_actualBalance -= it->actualBalance;
  m_pendingBalance -= it->pendingBalance;

  m_transfersSynchronizer.removeSubscription(pubAddr);

  deleteContainerFromUnlockTransactionJobs(it->container);
  std::vector<size_t> deletedTransactions;
  std::vector<size_t> updatedTransactions = deleteTransfersForAddress(address, deletedTransactions);
  deleteFromUncommitedTransactions(deletedTransactions);

  m_walletsContainer.get<KeysIndex>().erase(it);

  if (m_walletsContainer.get<RandomAccessIndex>().size() != 0) {
    startBlockchainSynchronizer();
  } else {
    m_blockHashesContainer.clear();
    m_blockHashesContainer.push_back(m_currency.genesisBlockHash());
  }

  for (size_t transactionIndex: updatedTransactions) {
    pushEvent(makeTransactionUpdatedEvent(transactionIndex));
  }
}

// Plan to remove
IFusionManager::EstimateResult WalletGreen::estimate(uint64_t threshold) const {
  throwIfNotInitialized();
  throwIfStopped();

  IFusionManager::EstimateResult result{0, 0};
  std::vector<WalletOuts> walletOuts = pickWalletsWithMoney();
  std::array<size_t, std::numeric_limits<uint64_t>::digits10 + 1> bucketSizes;
  bucketSizes.fill(0);
  for (size_t walletIndex = 0; walletIndex < walletOuts.size(); ++walletIndex) {
    for (auto& out : walletOuts[walletIndex].outs) {
      uint8_t powerOfTen = 0;
      if (m_currency.isAmountApplicableInFusionTransactionInput(out.amount, threshold, powerOfTen, m_node.getLastKnownBlockHeight())) {
        assert(powerOfTen < std::numeric_limits<uint64_t>::digits10 + 1);
        bucketSizes[powerOfTen]++;
      }
    }

    result.totalOutputCount += walletOuts[walletIndex].outs.size();
  }

  for (auto bucketSize : bucketSizes) {
    if (bucketSize >= m_currency.fusionTxMinInputCount()) {
      result.fusionReadyCount += bucketSize;
    }
  }

  return result;
}

uint64_t WalletGreen::getActualBalance() const
{

  throwIfNotInitialized();

  throwIfStopped();

  return m_actualBalance;
}

uint64_t WalletGreen::getActualBalance(const std::string& address) const
{
  throwIfNotInitialized();

  throwIfStopped();

  const WalletRecord& walletRecord = getWalletRecord(address);

  return walletRecord.actualBalance;
}

std::string WalletGreen::getAddress(size_t index) const
{
  throwIfNotInitialized();

  throwIfStopped();

  if (index >= m_walletsContainer.get<RandomAccessIndex>().size())
  {
    throw std::system_error(make_error_code(std::errc::invalid_argument));
  }

  const WalletRecord& walletRecord = m_walletsContainer.get<RandomAccessIndex>()[index];

  AccountPublicAddress accountPublicAddress = {walletRecord.spendPublicKey, m_viewPublicKey};

  return m_currency.accountAddressAsString(accountPublicAddress);
}

size_t WalletGreen::getAddressCount() const
{
  throwIfNotInitialized();

  throwIfStopped();

  return m_walletsContainer.get<RandomAccessIndex>().size();
}

KeyPair WalletGreen::getAddressSpendKeyPair(size_t index) const
{
  throwIfNotInitialized();

  throwIfStopped();

  if (index >= m_walletsContainer.get<RandomAccessIndex>().size())
  {
    throw std::system_error(make_error_code(std::errc::invalid_argument));
  }

  const WalletRecord& walletRecord = m_walletsContainer.get<RandomAccessIndex>()[index];

  KeyPair spendKeyPair = { walletRecord.spendPublicKey, walletRecord.spendSecretKey };

  return spendKeyPair;
}

KeyPair WalletGreen::getAddressSpendKeyPair(const std::string& address) const
{
  throwIfNotInitialized();

  throwIfStopped();

  AccountPublicAddress pubAddr = parseAddress(address);

  auto it = m_walletsContainer.get<KeysIndex>().find(pubAddr.spendPublicKey);

  if (it == m_walletsContainer.get<KeysIndex>().end()) {
    throw std::system_error(make_error_code(error::OBJECT_NOT_FOUND));
  }

  KeyPair spendKeyPair = { it->spendPublicKey, it->spendSecretKey };

  return spendKeyPair;
}

uint32_t WalletGreen::getBlockCount() const
{
  throwIfNotInitialized();
  
  throwIfStopped();

  uint32_t blockCount = static_cast<uint32_t>(m_blockHashesContainer.size());

  assert(blockCount != 0);

  return blockCount;
}

std::vector<Crypto::Hash> WalletGreen::getBlockHashes(uint32_t startBlockIndex, size_t count) const
{
  throwIfNotInitialized();
  
  throwIfStopped();

  auto& index = m_blockHashesContainer.get<BlockHeightIndex>();

  if (startBlockIndex >= index.size())
  {
    return std::vector<Crypto::Hash>();
  }

  auto start = std::next(index.begin(), startBlockIndex);
  auto end = std::next(index.begin(), std::min(index.size(), startBlockIndex + count));

  return std::vector<Crypto::Hash>(start, end);
}

std::vector<size_t> WalletGreen::getDelayedTransactionIndexes() const
{
  throwIfNotInitialized();

  throwIfStopped();

  throwIfTrackingMode();

  std::vector<size_t> transactionIndexes;
  transactionIndexes.reserve(m_uncommitedTransactions.size());

  for (const std::pair<size_t, CryptoNote::Transaction>& kv: m_uncommitedTransactions)
  {
    transactionIndexes.push_back(kv.first);
  }

  return transactionIndexes;
}

WalletEvent WalletGreen::getEvent()
{
  throwIfNotInitialized();
  
  throwIfStopped();

  while(m_eventsQueue.empty()) {
    m_eventOccurred.wait();
    m_eventOccurred.clear();
    throwIfStopped();
  }

  WalletEvent event = std::move(m_eventsQueue.front());
  m_eventsQueue.pop();

  return event;
}

uint64_t WalletGreen::getPendingBalance() const
{
  throwIfNotInitialized();

  throwIfStopped();

  return m_pendingBalance;
}

uint64_t WalletGreen::getPendingBalance(const std::string& address) const
{
  throwIfNotInitialized();

  throwIfStopped();

  const WalletRecord& walletRecord = getWalletRecord(address);

  return walletRecord.pendingBalance;
}

WalletTransactionWithTransfers WalletGreen::getTransaction(const Crypto::Hash& transactionHash) const {
  throwIfNotInitialized();
  
  throwIfStopped();

  auto& hashIndex = m_walletTransactions.get<TransactionIndex>();
  auto it = hashIndex.find(transactionHash);
  if (it == hashIndex.end())
  {
    throw std::system_error(make_error_code(error::OBJECT_NOT_FOUND), "Transaction not found");
  }

  WalletTransactionWithTransfers walletTransaction;
  walletTransaction.transaction = *it;
  walletTransaction.transfers = getTransactionTransfers(*it);

  return walletTransaction;
}

WalletTransaction WalletGreen::getTransaction(size_t transactionIndex) const
{
  throwIfNotInitialized();
  
  throwIfStopped();

  if (transactionIndex >= m_walletTransactions.size())
  {
    throw std::system_error(make_error_code(error::INDEX_OUT_OF_RANGE));
  }

  return m_walletTransactions.get<RandomAccessIndex>()[transactionIndex];
}

size_t WalletGreen::getTransactionCount() const
{
  throwIfNotInitialized();

  throwIfStopped();

  return m_walletTransactions.get<RandomAccessIndex>().size();
}

Crypto::SecretKey WalletGreen::getTransactionSecretKey(size_t transactionIndex) const
{
	throwIfNotInitialized();

	throwIfStopped();

	if (transactionIndex >= m_walletTransactions.size())
  {
		throw std::system_error(make_error_code(error::INDEX_OUT_OF_RANGE));
	}

  return m_walletTransactions.get<RandomAccessIndex>()[transactionIndex].secretKey.get();
}                                                                                      

WalletTransfer WalletGreen::getTransactionTransfer(size_t transactionIndex, size_t transferIndex) const
{
  throwIfNotInitialized();

  throwIfStopped();

  TransfersRange bounds = getTransactionTransfersRange(transactionIndex);

  if (transferIndex >= static_cast<size_t>(std::distance(bounds.first, bounds.second)))
  {
    throw std::system_error(make_error_code(std::errc::invalid_argument));
  }

  return std::next(bounds.first, transferIndex)->second;
}

size_t WalletGreen::getTransactionTransferCount(size_t transactionIndex) const
{
  throwIfNotInitialized();

  throwIfStopped();

  TransfersRange bounds = getTransactionTransfersRange(transactionIndex);

  return static_cast<size_t>(std::distance(bounds.first, bounds.second));
}

std::vector<TransactionsInBlockInfo> WalletGreen::getTransactions(const Crypto::Hash& blockHash, size_t count) const
{
  throwIfNotInitialized();

  throwIfStopped();

  // get block height/index from block hash

  auto& hashIndex = m_blockHashesContainer.get<BlockHashIndex>();
  auto it = hashIndex.find(blockHash);
  if (it == hashIndex.end())
  {
    return std::vector<TransactionsInBlockInfo>();
  }

  auto heightIt = m_blockHashesContainer.project<BlockHeightIndex>(it);

  uint32_t startBlockIndex = static_cast<uint32_t>(std::distance(m_blockHashesContainer.get<BlockHeightIndex>().begin(), heightIt));

  return getTransactionsInBlocks(startBlockIndex, count);
}

std::vector<TransactionsInBlockInfo> WalletGreen::getTransactions(uint32_t startBlockIndex, size_t count) const
{
  throwIfNotInitialized();

  throwIfStopped();

  return getTransactionsInBlocks(startBlockIndex, count);
}

std::vector<WalletTransactionWithTransfers> WalletGreen::getUnconfirmedTransactions() const
{
  throwIfNotInitialized();

  throwIfStopped();

  auto lowerBound = m_walletTransactions.get<BlockHeightIndex>().lower_bound(WALLET_UNCONFIRMED_TRANSACTION_HEIGHT); // find first instance where block's height is WALLET_UNCONFIRMED_TRANSACTION_HEIGHT
  std::vector<WalletTransactionWithTransfers> result;
  
  // loop over all unconfirmed transactions
  for (auto it = lowerBound; it != m_walletTransactions.get<BlockHeightIndex>().end(); ++it)
  {
    if (it->state != WalletTransactionState::SUCCEEDED) {
      continue;
    }

    WalletTransactionWithTransfers transaction;
    transaction.transaction = *it;
    transaction.transfers = getTransactionTransfers(*it);

    result.push_back(transaction);
  }

  return result;
}

KeyPair WalletGreen::getViewKeyPair() const
{
  throwIfNotInitialized();
  
  throwIfStopped();

  KeyPair viewKeyPair = { m_viewPublicKey, m_viewPrivateKey };

  return viewKeyPair;
}

void WalletGreen::initialize(const std::string& password)
{
  Crypto::PublicKey viewPublicKey;
  Crypto::SecretKey viewPrivateKey;
  Crypto::generate_keys(viewPublicKey, viewPrivateKey);

  initWithKeys(viewPublicKey, viewPrivateKey, password);
}

void WalletGreen::initializeWithViewKey(const Crypto::SecretKey& viewPrivateKey, const std::string& password)
{
  Crypto::PublicKey viewPublicKey;
  if (!Crypto::secret_key_to_public_key(viewPrivateKey, viewPublicKey))
  {
    throw std::system_error(make_error_code(error::KEY_GENERATION_ERROR));
  }

  initWithKeys(viewPublicKey, viewPrivateKey, password);
}

// Plan to remove
bool WalletGreen::isFusionTransaction(size_t transactionIndex) const
{
  throwIfNotInitialized();
  
  throwIfStopped();

  if (transactionIndex >= m_walletTransactions.size())
  {
    throw std::system_error(make_error_code(error::INDEX_OUT_OF_RANGE));
  }

  auto isFusionIter = m_fusionTxsCache.find(transactionIndex);
  if (isFusionIter != m_fusionTxsCache.end()) {
    return isFusionIter->second;
  }

  bool result = isFusionTransaction(m_walletTransactions.get<RandomAccessIndex>()[transactionIndex]);
  m_fusionTxsCache.emplace(transactionIndex, result);
  return result;
}

void WalletGreen::load(std::istream& source, const std::string& password)
{

  // load from disk to memory

  if (m_walletState != WalletState::NOT_INITIALIZED)
  {
    throw std::system_error(make_error_code(error::WRONG_STATE));
  }

  throwIfStopped();

  stopBlockchainSynchronizer();

  // This is for fixing the burning bug
  // Read all output keys in the cache
  try {
    std::vector<AccountPublicAddress> subscriptionList;
    m_transfersSynchronizer.getSubscriptions(subscriptionList);
    for (AccountPublicAddress& address : subscriptionList) {
      ITransfersSubscription* subscription = m_transfersSynchronizer.getSubscription(address);
      if (subscription != nullptr) {
        std::vector<TransactionOutputInformation> allTransfers;
        ITransfersContainer* container = &subscription->getContainer();
        container->getOutputs(allTransfers, ITransfersContainer::IncludeAll);
        for (TransactionOutputInformation& transactionOutput : allTransfers) {
          if (transactionOutput.type != TransactionTypes::OutputType::Invalid) {
            m_transfersSynchronizer.addPublicKeysSeen(address, transactionOutput.transactionHash, transactionOutput.outputKey);
          }
        }
      }
    }
  } catch (const std::exception& e) {
    // failed to read output keys
    throw;
  }

  unsafeLoad(source, password);

  assert(m_blockHashesContainer.empty());
  if (m_walletsContainer.get<RandomAccessIndex>().size() != 0) {
    m_transfersSynchronizer.subscribeConsumerNotifications(m_viewPublicKey, this);
    updateBlockHashesContainerWithViewKey(m_viewPublicKey);

    startBlockchainSynchronizer();
  } else {
    m_blockHashesContainer.push_back(m_currency.genesisBlockHash());
  }

  m_walletState = WalletState::INITIALIZED;
}

size_t WalletGreen::makeTransaction(const TransactionParameters& sendingTransactionParameters)
{
  throwIfNotInitialized();
  throwIfTrackingMode();
  throwIfStopped();

  Tools::ScopeExit releaseContext([this] {
    m_dispatcher.yield();
  });

  System::EventLock lock(m_readyEvent);

  validateTransactionParameters(sendingTransactionParameters);

  AccountPublicAddress changeDestination = getChangeDestination(sendingTransactionParameters.changeDestination, sendingTransactionParameters.sourceAddresses);

  std::vector<WalletOuts> wallets;
  if (!sendingTransactionParameters.sourceAddresses.empty())
  {
    wallets = pickWallets(sendingTransactionParameters.sourceAddresses);
  }
  else
  {
    wallets = pickWalletsWithMoney();
  }

  PreparedTransaction preparedTransaction;
  Crypto::SecretKey transactionPrivateKey;                              
  prepareTransaction(std::move(wallets), sendingTransactionParameters.destinations, sendingTransactionParameters.fee, sendingTransactionParameters.mixIn, sendingTransactionParameters.extra, sendingTransactionParameters.unlockTimestamp, sendingTransactionParameters.donation, changeDestination, preparedTransaction, transactionPrivateKey);

  return validateSaveAndSendTransaction(*preparedTransaction.transaction, preparedTransaction.destinations, false, false);
}

void WalletGreen::rollbackUncommitedTransaction(size_t transactionIndex)
{
  Tools::ScopeExit releaseContext([this] {
    m_dispatcher.yield();
  });

  System::EventLock lockk(m_readyEvent);

  throwIfNotInitialized();
  throwIfStopped();
  throwIfTrackingMode();

  if (transactionIndex >= m_walletTransactions.size())
  {
    throw std::system_error(make_error_code(error::INDEX_OUT_OF_RANGE));
  }

  auto txIt = m_walletTransactions.get<RandomAccessIndex>().begin();
  std::advance(txIt, transactionIndex);
  if (m_uncommitedTransactions.count(transactionIndex) == 0 || txIt->state != WalletTransactionState::CREATED)
  {
    throw std::system_error(make_error_code(error::TX_CANCEL_IMPOSSIBLE));
  }

  removeUnconfirmedTransaction(getObjectHash(m_uncommitedTransactions[transactionIndex]));
  m_uncommitedTransactions.erase(transactionIndex);
}

void WalletGreen::save(std::ostream& destination, bool saveDetails, bool saveCache)
{
  throwIfNotInitialized();
  throwIfStopped();

  stopBlockchainSynchronizer();

  unsafeSave(destination, saveDetails, saveCache);

  startBlockchainSynchronizer();
}

void WalletGreen::shutdown()
{
  throwIfNotInitialized();

  doShutdown();

  m_dispatcher.yield(); //let remote spawns finish
}

void WalletGreen::start()
{
  m_stopped = false;
}

void WalletGreen::stop()
{
  m_stopped = true;
  m_eventOccurred.set();
}

size_t WalletGreen::transfer(const TransactionParameters& transactionParameters, Crypto::SecretKey& transactionPrivateKey)
{
  Tools::ScopeExit releaseContext([this] {
    m_dispatcher.yield();
  });

  System::EventLock lock(m_readyEvent);

  throwIfNotInitialized();
  throwIfTrackingMode();
  throwIfStopped();

  return doTransfer(transactionParameters, transactionPrivateKey);
}


// Private functions


void WalletGreen::addBlockHashes(const std::vector<Crypto::Hash>& blockHashes)
{
  System::EventLock lock(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED)
  {
    return;
  }

  m_blockHashesContainer.insert(m_blockHashesContainer.end(), blockHashes.begin(), blockHashes.end());
}

void WalletGreen::addUnconfirmedTransaction(const ITransactionReader& transaction)
{
  System::RemoteContext<std::error_code> context(m_dispatcher, [this, &transaction] {
    return m_blockchainSynchronizer.addUnconfirmedTransaction(transaction).get();
  });

  std::error_code error = context.get();
  if (error)
  {
    throw std::system_error(error, "Failed to add unconfirmed transaction");
  }
}

std::string WalletGreen::addWallet(const Crypto::PublicKey& spendPublicKey, const Crypto::SecretKey& spendPrivateKey, uint64_t creationTimestamp)
{
  auto& index = m_walletsContainer.get<KeysIndex>();

  WalletTrackingMode trackingMode = getTrackingMode();

  if ((trackingMode == WalletTrackingMode::TRACKING && spendPrivateKey != NULL_SECRET_KEY) ||
      (trackingMode == WalletTrackingMode::NOT_TRACKING && spendPrivateKey == NULL_SECRET_KEY))
  {
    throw std::system_error(make_error_code(error::BAD_ADDRESS));
  }

  auto insertIt = index.find(spendPublicKey);
  if (insertIt != index.end())
  {
    throw std::system_error(make_error_code(error::ADDRESS_ALREADY_EXISTS));
  }

  AccountSubscription subscription;
  subscription.keys.address.viewPublicKey = m_viewPublicKey;
  subscription.keys.address.spendPublicKey = spendPublicKey;
  subscription.keys.viewSecretKey = m_viewPrivateKey;
  subscription.keys.spendSecretKey = spendPrivateKey;
  subscription.transactionSpendableAge = m_transactionSoftLockTime;
  subscription.syncStart.height = 0;
  subscription.syncStart.timestamp = std::max(creationTimestamp, ACCOUNT_CREATE_TIME_ACCURACY) - ACCOUNT_CREATE_TIME_ACCURACY;

  ITransfersSubscription& transfersSubscription = m_transfersSynchronizer.addSubscription(subscription);
  ITransfersContainer* container = &transfersSubscription.getContainer();

  WalletRecord walletRecord;
  walletRecord.spendPublicKey = spendPublicKey;
  walletRecord.spendSecretKey = spendPrivateKey;
  walletRecord.container = container;
  walletRecord.creationTimestamp = static_cast<time_t>(creationTimestamp);
  transfersSubscription.addObserver(this);

  index.insert(insertIt, std::move(walletRecord));

  if (index.size() == 1)
  {
    m_transfersSynchronizer.subscribeConsumerNotifications(m_viewPublicKey, this);
    updateBlockHashesContainerWithViewKey(m_viewPublicKey);
  }

  AccountPublicAddress addressPublicKeys = { spendPublicKey, m_viewPublicKey };

  return m_currency.accountAddressAsString(addressPublicKeys);
}

bool WalletGreen::adjustTransfer(size_t transactionIndex, size_t firstTransferIndex, const std::string& address, int64_t amount) {
  assert(amount != 0);

  bool updated = false;
  bool updateOutputTransfers = amount > 0;
  bool firstAddressTransferFound = false;
  auto it = std::next(m_walletTransfers.begin(), firstTransferIndex);
  while (it != m_walletTransfers.end() && it->first == transactionIndex) {
    assert(it->second.amount != 0);
    bool transferIsOutput = it->second.amount > 0;
    if (transferIsOutput == updateOutputTransfers && it->second.address == address) {
      if (firstAddressTransferFound) {
        it = m_walletTransfers.erase(it);
        updated = true;
      } else {
        if (it->second.amount != amount) {
          it->second.amount = amount;
          updated = true;
        }
        
        firstAddressTransferFound = true;
        ++it;
      }
    } else {
      ++it;
    }
  }

  if (!firstAddressTransferFound) {
    WalletTransfer transfer{ WalletTransferType::USUAL, address, amount };
    m_walletTransfers.emplace(it, std::piecewise_construct, std::forward_as_tuple(transactionIndex), std::forward_as_tuple(transfer));
    updated = true;
  }

  return updated;
}

void WalletGreen::appendTransfer(size_t transactionIndex, size_t firstTransferIndex, const std::string& address, int64_t amount) {
  auto it = std::next(m_walletTransfers.begin(), firstTransferIndex);
  auto insertIt = std::upper_bound(it, m_walletTransfers.end(), transactionIndex, [](size_t transactionIndex, const TransactionTransferPair& pair) {
    return transactionIndex < pair.first;
  });

  WalletTransfer transfer{ WalletTransferType::USUAL, address, amount };
  m_walletTransfers.emplace(insertIt, std::piecewise_construct, std::forward_as_tuple(transactionIndex), std::forward_as_tuple(transfer));
}

void WalletGreen::blocksRollback(uint32_t blockIndex)
{
  System::EventLock lock(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED)
  {
    return;
  }

  auto& blockHashesContainer = m_blockHashesContainer.get<BlockHeightIndex>();

  blockHashesContainer.erase(std::next(blockHashesContainer.begin(), blockIndex), blockHashesContainer.end());
}

void WalletGreen::clearCaches()
{
  std::vector<AccountPublicAddress> subscriptions;
  m_transfersSynchronizer.getSubscriptions(subscriptions);
  std::for_each(subscriptions.begin(), subscriptions.end(), [this] (const AccountPublicAddress& address) { m_transfersSynchronizer.removeSubscription(address); });

  m_walletsContainer.clear();
  m_unlockTransactionsJob.clear();
  m_walletTransactions.clear();
  m_walletTransfers.clear();
  m_uncommitedTransactions.clear();
  m_actualBalance = 0;
  m_pendingBalance = 0;
  m_fusionTxsCache.clear();
  m_blockHashesContainer.clear();
}

uint64_t WalletGreen::countNeededMoney(const std::vector<CryptoNote::WalletTransfer>& destinations, uint64_t fee)
{
  uint64_t neededMoney = 0;
  for (const CryptoNote::WalletTransfer& transfer: destinations)
  {
    if (transfer.amount == 0)
    {
      throw std::system_error(make_error_code(CryptoNote::error::ZERO_DESTINATION));
    }
    else if (transfer.amount < 0)
    {
      throw std::system_error(make_error_code(std::errc::invalid_argument));
    }

    //to supress warning
    uint64_t uamount = static_cast<uint64_t>(transfer.amount);

    neededMoney += uamount;
    if (neededMoney < uamount) {
      throw std::system_error(make_error_code(CryptoNote::error::SUM_OVERFLOW));
    }
  }

  neededMoney += fee;
  if (neededMoney < fee)
  {
    throw std::system_error(make_error_code(CryptoNote::error::SUM_OVERFLOW));
  }

  return neededMoney;
}

// Plan to remove
WalletGreen::ReceiverAmounts WalletGreen::decomposeFusionOutputs(uint64_t inputsAmount) {
  assert(m_walletsContainer.get<RandomAccessIndex>().size() > 0);

  WalletGreen::ReceiverAmounts outputs;
  outputs.receiver = {m_walletsContainer.get<RandomAccessIndex>().begin()->spendPublicKey, m_viewPublicKey};

  decomposeAmount(inputsAmount, 0, outputs.amounts);
  std::sort(outputs.amounts.begin(), outputs.amounts.end());

  return outputs;
}

void WalletGreen::deleteContainerFromUnlockTransactionJobs(const ITransfersContainer* container)
{
  for (auto it = m_unlockTransactionsJob.begin(); it != m_unlockTransactionsJob.end();)
  {
    if (it->container == container)
    {
      it = m_unlockTransactionsJob.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void WalletGreen::deleteFromUncommitedTransactions(const std::vector<size_t>& deletedTransactionIndexes) {
  for (size_t transactionIndex : deletedTransactionIndexes)
  {
    m_uncommitedTransactions.erase(transactionIndex);
  }
}

std::vector<size_t> WalletGreen::deleteTransfersForAddress(const std::string& address, std::vector<size_t>& deletedTransactionIndexes) {
  assert(!address.empty());

  int64_t deletedInputs = 0;
  int64_t deletedOutputs = 0;

  int64_t unknownInputs = 0;

  bool transfersLeft = false;
  size_t firstTransactionTransfer = 0;

  std::vector<size_t> updatedTransactions;

  for (size_t i = 0; i < m_walletTransfers.size(); ++i) {
    WalletTransfer& transfer = m_walletTransfers[i].second;

    if (transfer.address == address) {
      if (transfer.amount >= 0) {
        deletedOutputs += transfer.amount;
      } else {
        deletedInputs += transfer.amount;
        transfer.address = "";
      }
    } else if (transfer.address.empty()) {
      if (transfer.amount < 0) {
        unknownInputs += transfer.amount;
      }
    } else if (isMyAddress(transfer.address)) {
      transfersLeft = true;
    }

    size_t transactionId = m_walletTransfers[i].first;
    if ((i == m_walletTransfers.size() - 1) || (transactionId != m_walletTransfers[i + 1].first)) {
      //the last transfer for current transaction

      size_t transfersBeforeMerge = m_walletTransfers.size();
      if (deletedInputs != 0) {
        adjustTransfer(transactionId, firstTransactionTransfer, "", deletedInputs + unknownInputs);
      }

      assert(transfersBeforeMerge >= m_walletTransfers.size());
      i -= transfersBeforeMerge - m_walletTransfers.size();

      auto& randomIndex = m_walletTransactions.get<RandomAccessIndex>();

      randomIndex.modify(std::next(randomIndex.begin(), transactionId), [transfersLeft, deletedInputs, deletedOutputs] (WalletTransaction& transaction) {
        transaction.totalAmount -= deletedInputs + deletedOutputs;

        if (!transfersLeft) {
          transaction.state = WalletTransactionState::DELETED;
        }
      });

      if (!transfersLeft) {
        deletedTransactionIndexes.push_back(transactionId);
      }

      if (deletedInputs != 0 || deletedOutputs != 0) {
        updatedTransactions.push_back(transactionId);
      }

      //reset values for next transaction
      deletedInputs = 0;
      deletedOutputs = 0;
      unknownInputs = 0;
      transfersLeft = false;
      firstTransactionTransfer = i + 1;
    }
  }

  return updatedTransactions;
}

void WalletGreen::deleteUnlockTransactionJob(const Crypto::Hash& transactionHash) {
  auto& index = m_unlockTransactionsJob.get<TransactionHashIndex>();
  index.erase(transactionHash);
}

std::string WalletGreen::doCreateAddress(const Crypto::PublicKey& spendPublicKey, const Crypto::SecretKey& spendPrivateKey, uint64_t creationTimestamp)
{
  
  assert(creationTimestamp <= std::numeric_limits<uint64_t>::max() - m_currency.blockFutureTimeLimit());

  throwIfNotInitialized();
  throwIfStopped();

  stopBlockchainSynchronizer();

  std::string address;
  try
  {
    address = addWallet(spendPublicKey, spendPrivateKey, creationTimestamp);
    uint64_t currentTime = static_cast<uint64_t>(time(nullptr));

    if (creationTimestamp + m_currency.blockFutureTimeLimit() < currentTime)
    {
      std::string password = m_password;
      std::stringstream ss;
      unsafeSave(ss, true, false);
      shutdown();
      load(ss, password);
    }
  }
  catch (std::exception&)
  {
    startBlockchainSynchronizer();
    throw;
  }

  startBlockchainSynchronizer();

  return address;
}

void WalletGreen::doShutdown()
{
  if (m_walletsContainer.size() != 0)
  {
    m_transfersSynchronizer.unsubscribeConsumerNotifications(m_viewPublicKey, this);
  }

  stopBlockchainSynchronizer();
  
  m_blockchainSynchronizer.removeObserver(this);

  clearCaches();

  std::queue<WalletEvent> noEvents;
  std::swap(m_eventsQueue, noEvents);

  m_walletState = WalletState::NOT_INITIALIZED;
}

size_t WalletGreen::doTransfer(const TransactionParameters& transactionParameters, Crypto::SecretKey& transactionPrivateKey)
{
  validateTransactionParameters(transactionParameters);
  AccountPublicAddress changeDestination = getChangeDestination(transactionParameters.changeDestination, transactionParameters.sourceAddresses);

  std::vector<WalletOuts> wallets;
  if (!transactionParameters.sourceAddresses.empty())
  {
    wallets = pickWallets(transactionParameters.sourceAddresses);
  }
  else
  {
    wallets = pickWalletsWithMoney();
  }

  PreparedTransaction preparedTransaction;
  prepareTransaction(std::move(wallets),
    transactionParameters.destinations,
    transactionParameters.fee,
    transactionParameters.mixIn,
    transactionParameters.extra,
    transactionParameters.unlockTimestamp,
    transactionParameters.donation,
    changeDestination,
    preparedTransaction,
    transactionPrivateKey);

  return validateSaveAndSendTransaction(*preparedTransaction.transaction, preparedTransaction.destinations, false, true);
}

bool WalletGreen::eraseForeignTransfers(size_t transactionIndex, size_t firstTransferIndex, const std::unordered_set<std::string>& knownAddresses, bool eraseOutputTransfers) {

  return eraseTransfers(transactionIndex, firstTransferIndex, [this, &knownAddresses, eraseOutputTransfers](bool isOutput, const std::string& transferAddress) {
    return eraseOutputTransfers == isOutput && knownAddresses.count(transferAddress) == 0;
  });
}

bool WalletGreen::eraseTransfers(size_t transactionIndex, size_t firstTransferIndex, std::function<bool(bool, const std::string&)>&& predicate) {
  bool erased = false;
  auto it = std::next(m_walletTransfers.begin(), firstTransferIndex);
  while (it != m_walletTransfers.end() && it->first == transactionIndex) {
    bool transferIsOutput = it->second.amount > 0;
    if (predicate(transferIsOutput, it->second.address)) {
      it = m_walletTransfers.erase(it);
      erased = true;
    } else {
      ++it;
    }
  }

  return erased;
}

bool WalletGreen::eraseTransfersByAddress(size_t transactionIndex, size_t firstTransferIndex, const std::string& address, bool eraseOutputTransfers) {
  return eraseTransfers(transactionIndex, firstTransferIndex, [&address, eraseOutputTransfers](bool isOutput, const std::string& transferAddress) {
    return eraseOutputTransfers == isOutput && address == transferAddress;
  });
}

void WalletGreen::filterOutTransactions(WalletTransactions& transactions, WalletTransfers& transfers, std::function<bool (const WalletTransaction&)>&& pred) const
{
  size_t cancelledTransactions = 0;

  auto& index = m_walletTransactions.get<RandomAccessIndex>();
  for (size_t i = 0; i < m_walletTransactions.size(); ++i) {
    const WalletTransaction& transaction = index[i];

    if (pred(transaction)) {
      ++cancelledTransactions;
      continue;
    }

    transactions.push_back(transaction);
    std::vector<WalletTransfer> transactionTransfers = getTransactionTransfers(transaction);
    for (WalletTransfer& transfer: transactionTransfers) {
      transfers.push_back(TransactionTransferPair {i - cancelledTransactions, std::move(transfer)} );
    }
  }
}

Crypto::Hash WalletGreen::getBlockHashByIndex(uint32_t blockIndex) const
{
  assert(blockIndex < m_blockHashesContainer.size());
  return m_blockHashesContainer.get<BlockHeightIndex>()[blockIndex];
}

AccountPublicAddress WalletGreen::getChangeDestination(const std::string& changeDestinationAddress, const std::vector<std::string>& sourceAddresses) const
{

  // changeDestinationAddress must belong to current container
  // sourceAddresses must belong to current container

  if (!changeDestinationAddress.empty())
  {
    return parseAccountAddressString(changeDestinationAddress, m_currency);
  }

  if (m_walletsContainer.size() == 1)
  {
    AccountPublicAddress address = { m_walletsContainer.get<RandomAccessIndex>()[0].spendPublicKey, m_viewPublicKey };
    return address;
  }

  assert(sourceAddresses.size() == 1 && isMyAddress(sourceAddresses[0]));

  return parseAccountAddressString(sourceAddresses[0], m_currency);
}

WalletGreen::TransfersMap WalletGreen::getKnownTransfersMap(size_t transactionIndex, size_t firstTransferIndex) const
{
  TransfersMap transfersMap; // { addressStr, { inputAmount, outputAmount } }

  for (auto it = std::next(m_walletTransfers.begin(), firstTransferIndex); it != m_walletTransfers.end() && it->first == transactionIndex; ++it) {
    WalletTransfer walletTransfer = it->second;

    const std::string& address = walletTransfer.address;
    const int64_t& amount = walletTransfer.amount;

    if (!address.empty()) {
      if (amount < 0)
      {
        transfersMap[address].input += amount;
      }
      else
      {
        assert(amount > 0);
        transfersMap[address].output += amount;
      }
    }
  }

  return transfersMap;
}

WalletGreen::WalletTrackingMode WalletGreen::getTrackingMode() const
{
  if (m_walletsContainer.get<RandomAccessIndex>().empty())
  {
    return WalletTrackingMode::NO_ADDRESSES;
  }

  return m_walletsContainer.get<RandomAccessIndex>().begin()->spendSecretKey == NULL_SECRET_KEY ? WalletTrackingMode::TRACKING : WalletTrackingMode::NOT_TRACKING;
}

size_t WalletGreen::getTransactionIndex(const Crypto::Hash& transactionHash) const
{
  auto it = m_walletTransactions.get<TransactionIndex>().find(transactionHash);

  if (it == m_walletTransactions.get<TransactionIndex>().end())
  {
    throw std::system_error(make_error_code(std::errc::invalid_argument));
  }

  auto randomIt = m_walletTransactions.project<RandomAccessIndex>(it);
  auto transactionIndex = std::distance(m_walletTransactions.get<RandomAccessIndex>().begin(), randomIt);

  return transactionIndex;
}

std::vector<WalletTransfer> WalletGreen::getTransactionTransfers(const WalletTransaction& transaction) const
{
  auto& walletTransactions = m_walletTransactions.get<RandomAccessIndex>();

  auto it = walletTransactions.iterator_to(transaction);

  assert(it != walletTransactions.end());

  size_t transactionIndex = std::distance(walletTransactions.begin(), it);
  size_t transfersCount = getTransactionTransferCount(transactionIndex);

  std::vector<WalletTransfer> walletTransfers;
  walletTransfers.reserve(transfersCount);

  for (size_t i = 0; i < transfersCount; ++i)
  {
    walletTransfers.push_back(getTransactionTransfer(transactionIndex, i));
  }

  return walletTransfers;
}

WalletGreen::TransfersRange WalletGreen::getTransactionTransfersRange(size_t transactionIndex) const {
  std::pair<size_t, WalletTransfer> val = std::make_pair(transactionIndex, WalletTransfer());

  auto bounds = std::equal_range(m_walletTransfers.begin(), m_walletTransfers.end(), val, [] (const TransactionTransferPair& a, const TransactionTransferPair& b) {
    return a.first < b.first;
  });

  return bounds;
}

std::vector<TransactionsInBlockInfo> WalletGreen::getTransactionsInBlocks(uint32_t startBlockIndex, size_t count) const
{
  if (count == 0)
  {
    throw std::system_error(make_error_code(error::WRONG_PARAMETERS), "blocks count must be greater than zero");
  }

  std::vector<TransactionsInBlockInfo> result;

  if (startBlockIndex >= m_blockHashesContainer.size())
  {
    return result;
  }

  auto& walletTransactions = m_walletTransactions.get<BlockHeightIndex>();
  uint32_t stopIndex = static_cast<uint32_t>(std::min(m_blockHashesContainer.size(), startBlockIndex + count));

  for (uint32_t i = startBlockIndex; i < stopIndex; ++i)
  {
    TransactionsInBlockInfo info;

    info.blockHash = m_blockHashesContainer[i];

    auto lowerBound = walletTransactions.lower_bound(i);
    auto upperBound = walletTransactions.upper_bound(i);
    for (auto it = lowerBound; it != upperBound; ++it)
    {
      if (it->state == WalletTransactionState::SUCCEEDED)
      {
        WalletTransactionWithTransfers transactionWithTransfers;
        transactionWithTransfers.transaction = *it;

        transactionWithTransfers.transfers = getTransactionTransfers(*it);

        info.transactions.emplace_back(std::move(transactionWithTransfers));
      }
    }

    result.emplace_back(std::move(info));
  }

  return result;
}

const WalletRecord& WalletGreen::getWalletRecord(const Crypto::PublicKey& spendPublicKey) const
{
  auto it = m_walletsContainer.get<KeysIndex>().find(spendPublicKey);
  
  if (it == m_walletsContainer.get<KeysIndex>().end())
  {
    throw std::system_error(make_error_code(error::WALLET_NOT_FOUND));
  }

  return *it;
}

const WalletRecord& WalletGreen::getWalletRecord(const std::string& address) const
{
  AccountPublicAddress addressPublicKeys = parseAddress(address);
  return getWalletRecord(addressPublicKeys.spendPublicKey);
}

const WalletRecord& WalletGreen::getWalletRecord(ITransfersContainer* container) const
{
  auto it = m_walletsContainer.get<TransfersContainerIndex>().find(container);

  if (it == m_walletsContainer.get<TransfersContainerIndex>().end())
  {
    throw std::system_error(make_error_code(error::WALLET_NOT_FOUND));
  }

  return *it;
}

void WalletGreen::initWithKeys(const Crypto::PublicKey& viewPublicKey, const Crypto::SecretKey& viewPrivateKey, const std::string& password)
{
  // throw error if WalletGreen is already initialized
  if (m_walletState != WalletState::NOT_INITIALIZED)
  {
    throw std::system_error(make_error_code(error::ALREADY_INITIALIZED));
  }

  throwIfStopped();

  m_viewPublicKey = viewPublicKey;
  m_viewPrivateKey = viewPrivateKey;
  m_password = password;

  assert(m_blockHashesContainer.empty());

  m_blockHashesContainer.push_back(m_currency.genesisBlockHash());

  m_blockchainSynchronizer.addObserver(this);

  m_walletState = WalletState::INITIALIZED;
}

size_t WalletGreen::insertBlockchainTransaction(const TransactionInformation& txInfo, int64_t txTotalAmount)
{

  auto& index = m_walletTransactions.get<RandomAccessIndex>();

  WalletTransaction tx;
  tx.state = WalletTransactionState::SUCCEEDED;
  tx.timestamp = txInfo.timestamp;
  tx.blockHeight = txInfo.blockHeight;
  tx.hash = txInfo.transactionHash;
  tx.isBase = txInfo.totalAmountIn == 0;
  if (tx.isBase) {
    tx.fee = 0;
  } else {
    tx.fee = txInfo.totalAmountIn - txInfo.totalAmountOut;
  }

  tx.unlockTime = txInfo.unlockTime;
  tx.extra.assign(reinterpret_cast<const char*>(txInfo.extra.data()), txInfo.extra.size());
  tx.totalAmount = txTotalAmount;
  tx.creationTime = txInfo.timestamp;

  size_t transactionIndex = index.size();
  index.push_back(std::move(tx));

  return transactionIndex;
}

size_t WalletGreen::insertOutgoingTransactionAndPushEvent(const Crypto::Hash& transactionHash, uint64_t fee, const BinaryArray& extra, uint64_t unlockTimestamp, Crypto::SecretKey& txPrivateKey)
{
  WalletTransaction tx;
  tx.state = WalletTransactionState::CREATED;
  tx.creationTime = static_cast<uint64_t>(time(nullptr));
  tx.unlockTime = unlockTimestamp;
  tx.blockHeight = WALLET_UNCONFIRMED_TRANSACTION_HEIGHT;
  tx.extra.assign(reinterpret_cast<const char*>(extra.data()), extra.size());
  tx.fee = fee;
  tx.hash = transactionHash;
  tx.totalAmount = 0; // 0 until transactionHandlingEnd() is called
  tx.timestamp = 0; //0 until included in a block
  tx.isBase = false;
  tx.secretKey = txPrivateKey;

  size_t transactionIndex = m_walletTransactions.get<RandomAccessIndex>().size();
  m_walletTransactions.get<RandomAccessIndex>().push_back(std::move(tx));

  CryptoNote::WalletEvent event;
  event.type = CryptoNote::WalletEventType::TRANSACTION_CREATED;
  event.transactionCreated.transactionIndex = transactionIndex;

  pushEvent(event);

  return transactionIndex;
}

void WalletGreen::insertUnlockTransactionJob(const Crypto::Hash& transactionHash, uint32_t blockHeight, ITransfersContainer* container)
{
  auto& unlockTransactionsJob = m_unlockTransactionsJob.get<BlockHeightIndex>();
  unlockTransactionsJob.insert( { blockHeight, container, transactionHash } );
}

// Plan to remove
bool WalletGreen::isFusionTransaction(const WalletTransaction& walletTx) const {
  if (walletTx.fee != 0) {
    return false;
  }

  uint64_t inputsSum = 0;
  uint64_t outputsSum = 0;
  std::vector<uint64_t> outputsAmounts;
  std::vector<uint64_t> inputsAmounts;
  TransactionInformation txInfo;
  bool gotTx = false;
  const auto& walletsIndex = m_walletsContainer.get<RandomAccessIndex>();
  for (const WalletRecord& wallet : walletsIndex) {
    for (const TransactionOutputInformation& output : wallet.container->getTransactionOutputs(walletTx.hash, ITransfersContainer::IncludeTypeKey | ITransfersContainer::IncludeStateAll)) {
      if (outputsAmounts.size() <= output.outputInTransaction) {
        outputsAmounts.resize(output.outputInTransaction + 1, 0);
      }

      assert(output.amount != 0);
      assert(outputsAmounts[output.outputInTransaction] == 0);
      outputsAmounts[output.outputInTransaction] = output.amount;
      outputsSum += output.amount;
    }

    for (const TransactionOutputInformation& input : wallet.container->getTransactionInputs(walletTx.hash, ITransfersContainer::IncludeTypeKey)) {
      inputsSum += input.amount;
      inputsAmounts.push_back(input.amount);
    }

    if (!gotTx) {
      gotTx = wallet.container->getTransactionInformation(walletTx.hash, txInfo);
    }
  }

  if (!gotTx) {
    return false;
  }

  if (outputsSum != inputsSum || outputsSum != txInfo.totalAmountOut || inputsSum != txInfo.totalAmountIn) {
    return false;
  } else {
    return m_currency.isFusionTransaction(inputsAmounts, outputsAmounts, 0, m_node.getLastKnownBlockHeight()); //size = 0 here because can't get real size of tx in wallet.
  }
}

bool WalletGreen::isMyAddress(const std::string& addressString) const
{
  AccountPublicAddress address = parseAccountAddressString(addressString, m_currency);
  return m_viewPublicKey == address.viewPublicKey && m_walletsContainer.get<KeysIndex>().count(address.spendPublicKey) != 0;
}

AccountKeys WalletGreen::makeAccountKeys(const WalletRecord& walletRecord) const
{
  AccountKeys accountKeys;
  accountKeys.address.spendPublicKey = walletRecord.spendPublicKey;
  accountKeys.address.viewPublicKey = m_viewPublicKey;
  accountKeys.spendSecretKey = walletRecord.spendSecretKey;
  accountKeys.viewSecretKey = m_viewPrivateKey;

  return accountKeys;
}

std::unique_ptr<ITransaction> WalletGreen::makeTransaction(const std::vector<ReceiverAmounts>& decomposedOutputs, std::vector<InputInfo>& keysInfo, const std::string& extra, uint64_t unlockTimestamp, Crypto::SecretKey& transactionPrivateKey) {

  std::unique_ptr<ITransaction> transactionPtr = createTransaction();

  typedef std::pair<const AccountPublicAddress*, uint64_t> AmountToAddress;
  std::vector<AmountToAddress> amountsToAddresses;
  for (const ReceiverAmounts& output : decomposedOutputs) {
    for (uint64_t amount : output.amounts) {
      amountsToAddresses.emplace_back(AmountToAddress { &output.receiver, amount });
    }
  }

  std::shuffle(amountsToAddresses.begin(), amountsToAddresses.end(), std::default_random_engine{Crypto::rand<std::default_random_engine::result_type>()});
  std::sort(amountsToAddresses.begin(), amountsToAddresses.end(), [] (const AmountToAddress& left, const AmountToAddress& right) {
    return left.second < right.second;
  });

  for (const AmountToAddress& amountToAddress: amountsToAddresses) {
    transactionPtr->addOutput(amountToAddress.second, *amountToAddress.first);
  }

  transactionPtr->setUnlockTime(unlockTimestamp);
  transactionPtr->appendExtra(Common::asBinaryArray(extra));

  for (InputInfo& input : keysInfo) {
    transactionPtr->addInput(makeAccountKeys(*input.walletRecord), input.keyInfo, input.ephKeys);
  }

  size_t i = 0;
  for(InputInfo& input : keysInfo) {
    transactionPtr->signInputKey(i++, input.keyInfo, input.ephKeys);
  }

  Crypto::SecretKey txPirvateKey;
  transactionPtr->getTransactionSecretKey(txPirvateKey);
  transactionPrivateKey = txPirvateKey;

  return transactionPtr;
}

void WalletGreen::onBlockchainDetach(const Crypto::PublicKey& viewPublicKey, uint32_t blockIndex)
{
  m_dispatcher.remoteSpawn([this, blockIndex] () { blocksRollback(blockIndex); } );
}

void WalletGreen::onBlocksAdded(const Crypto::PublicKey& viewPublicKey, const std::vector<Crypto::Hash>& blockHashes)
{
  m_dispatcher.remoteSpawn([this, blockHashes] () { addBlockHashes(blockHashes); } );
}

void WalletGreen::onError(ITransfersSubscription* object, uint32_t height, std::error_code ec)
{
}

void WalletGreen::onSynchronizationCompleted()
{
  System::EventLock lock(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED)
  {
    return;
  }

  WalletEvent syncCompletedEvent;
  syncCompletedEvent.type = CryptoNote::WalletEventType::SYNC_COMPLETED;

  pushEvent(syncCompletedEvent);
}

void WalletGreen::onSynchronizationProgressUpdated(uint32_t processedBlockCount, uint32_t totalBlockCount)
{
  assert(processedBlockCount > 0);

  System::EventLock lock(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED)
  {
    return;
  }

  WalletEvent syncProgressUpdatedEvent;
  syncProgressUpdatedEvent.type = WalletEventType::SYNC_PROGRESS_UPDATED;
  syncProgressUpdatedEvent.synchronizationProgressUpdated.processedBlockCount = processedBlockCount;
  syncProgressUpdatedEvent.synchronizationProgressUpdated.totalBlockCount = totalBlockCount;

  pushEvent(syncProgressUpdatedEvent);

  uint32_t currentHeight = processedBlockCount - 1; // should currentHeight be renamed to currentBlockIndex?
  unlockBalances(currentHeight);

  m_refresh_progress_reporter.update(processedBlockCount, false);
}

// Plan to make this function empty because function transactionDeleteBegin() is empty
void WalletGreen::onTransactionDeleteBegin(const Crypto::PublicKey& viewPublicKey, Crypto::Hash transactionHash)
{
  m_dispatcher.remoteSpawn([=]() { transactionDeleteBegin(transactionHash); });
}

// Plan to make this function empty because function transactionDeleteEnd() is empty
void WalletGreen::onTransactionDeleteEnd(const Crypto::PublicKey& viewPublicKey, Crypto::Hash transactionHash)
{
  m_dispatcher.remoteSpawn([=]() { transactionDeleteEnd(transactionHash); });
}

void WalletGreen::onTransactionDeleted(ITransfersSubscription* object, const Crypto::Hash& transactionHash) {
  m_dispatcher.remoteSpawn([object, transactionHash, this] () { this->transactionDeleted(object, transactionHash); });
}

void WalletGreen::onTransactionUpdated(ITransfersSubscription* /*object*/, const Crypto::Hash& /*transactionHash*/)
{
  // Deprecated, ignore it. New event handler is onTransactionUpdated(const Crypto::PublicKey&, const Crypto::Hash&, const std::vector<ITransfersContainer*>&)
}

void WalletGreen::onTransactionUpdated(const Crypto::PublicKey& viewPublicKey, const Crypto::Hash& transactionHash, const std::vector<ITransfersContainer*>& containers) {
  
  assert(!containers.empty());

  TransactionInformation info;
  std::vector<ContainerAmounts> containerAmountsList;
  containerAmountsList.reserve(containers.size());
  for (ITransfersContainer* containerPtr : containers) {
    uint64_t inputsAmount;
    // Don't move this code to the following remote spawn, because it guarantees that the container has the transaction
    uint64_t outputsAmount;
    bool found = containerPtr->getTransactionInformation(transactionHash, info, &inputsAmount, &outputsAmount);
    assert(found);

    ContainerAmounts containerAmounts;
    containerAmounts.container = containerPtr;
    containerAmounts.amounts.input = -static_cast<int64_t>(inputsAmount);
    containerAmounts.amounts.output = static_cast<int64_t>(outputsAmount);
    containerAmountsList.emplace_back(std::move(containerAmounts));
  }

  m_dispatcher.remoteSpawn([this, info, containerAmountsList] {
    this->transactionUpdated(info, containerAmountsList);
  });
}

AccountPublicAddress WalletGreen::parseAddress(const std::string& address) const
{
  AccountPublicAddress addressPublicKeys;

  if (!m_currency.parseAccountAddressString(address, addressPublicKeys))
  {
    throw std::system_error(make_error_code(error::BAD_ADDRESS));
  }

  return addressPublicKeys;
}

// Plan to remove
std::vector<WalletGreen::OutputToTransfer> WalletGreen::pickRandomFusionInputs(uint64_t threshold, size_t minInputCount, size_t maxInputCount) {
  std::vector<WalletGreen::OutputToTransfer> allFusionReadyOuts;
  auto walletOuts = pickWalletsWithMoney();
  std::array<size_t, std::numeric_limits<uint64_t>::digits10 + 1> bucketSizes;
  bucketSizes.fill(0);
  for (size_t walletIndex = 0; walletIndex < walletOuts.size(); ++walletIndex) {
    for (auto& out : walletOuts[walletIndex].outs) {
      uint8_t powerOfTen = 0;
      if (m_currency.isAmountApplicableInFusionTransactionInput(out.amount, threshold, powerOfTen, m_node.getLastKnownBlockHeight())) {
        allFusionReadyOuts.push_back({std::move(out), walletOuts[walletIndex].wallet});
        assert(powerOfTen < std::numeric_limits<uint64_t>::digits10 + 1);
        bucketSizes[powerOfTen]++;
      }
    }
  }

  //now, pick the bucket
  std::vector<uint8_t> bucketNumbers(bucketSizes.size());
  std::iota(bucketNumbers.begin(), bucketNumbers.end(), 0);
  std::shuffle(bucketNumbers.begin(), bucketNumbers.end(), std::default_random_engine{Crypto::rand<std::default_random_engine::result_type>()});
  size_t bucketNumberIndex = 0;
  for (; bucketNumberIndex < bucketNumbers.size(); ++bucketNumberIndex) {
    if (bucketSizes[bucketNumbers[bucketNumberIndex]] >= minInputCount) {
      break;
    }
  }
  
  if (bucketNumberIndex == bucketNumbers.size()) {
    return {};
  }

  size_t selectedBucket = bucketNumbers[bucketNumberIndex];
  assert(selectedBucket < std::numeric_limits<uint64_t>::digits10 + 1);
  assert(bucketSizes[selectedBucket] >= minInputCount);
  uint64_t lowerBound = 1;
  for (size_t i = 0; i < selectedBucket; ++i) {
    lowerBound *= 10;
  }
   
  uint64_t upperBound = selectedBucket == std::numeric_limits<uint64_t>::digits10 ? UINT64_MAX : lowerBound * 10;
  std::vector<WalletGreen::OutputToTransfer> selectedOuts;
  selectedOuts.reserve(bucketSizes[selectedBucket]);
  for (size_t outIndex = 0; outIndex < allFusionReadyOuts.size(); ++outIndex) {
    if (allFusionReadyOuts[outIndex].out.amount >= lowerBound && allFusionReadyOuts[outIndex].out.amount < upperBound) {
      selectedOuts.push_back(std::move(allFusionReadyOuts[outIndex]));
    }
  }

  assert(selectedOuts.size() >= minInputCount);

  auto outputsSortingFunction = [](const OutputToTransfer& l, const OutputToTransfer& r) { return l.out.amount < r.out.amount; };
  if (selectedOuts.size() <= maxInputCount) {
    std::sort(selectedOuts.begin(), selectedOuts.end(), outputsSortingFunction);
    return selectedOuts;
  }

  ShuffleGenerator<size_t, Crypto::random_engine<size_t>> generator(selectedOuts.size());
  std::vector<WalletGreen::OutputToTransfer> trimmedSelectedOuts;
  trimmedSelectedOuts.reserve(maxInputCount);
  for (size_t i = 0; i < maxInputCount; ++i) {
    trimmedSelectedOuts.push_back(std::move(selectedOuts[generator()]));
  }

  std::sort(trimmedSelectedOuts.begin(), trimmedSelectedOuts.end(), outputsSortingFunction);
  return trimmedSelectedOuts;  
}

WalletGreen::WalletOuts WalletGreen::pickWallet(const std::string& address) {
  const WalletRecord& walletRecord = getWalletRecord(address);

  ITransfersContainer* containerPtr = walletRecord.container;
  WalletOuts walletOuts;
  containerPtr->getOutputs(walletOuts.outs, ITransfersContainer::IncludeKeyUnlocked);
  walletOuts.wallet = const_cast<WalletRecord *>(&walletRecord);

  return walletOuts;
}

std::vector<WalletGreen::WalletOuts> WalletGreen::pickWallets(const std::vector<std::string>& addresses) {
  std::vector<WalletOuts> walletOuts;
  walletOuts.reserve(addresses.size());

  for (const std::string& address: addresses) {
    WalletOuts wallet = pickWallet(address);
    if (!wallet.outs.empty()) {
      walletOuts.emplace_back(std::move(wallet));
    }
  }

  return walletOuts;
}

std::vector<WalletGreen::WalletOuts> WalletGreen::pickWalletsWithMoney() const {
  auto& walletsContainer = m_walletsContainer.get<RandomAccessIndex>();

  std::vector<WalletOuts> walletOutsVect;
  for (const WalletRecord& walletRecord: walletsContainer) {
    if (walletRecord.actualBalance != 0) {

      ITransfersContainer* container = walletRecord.container;

      WalletOuts walletOuts;
      container->getOutputs(walletOuts.outs, ITransfersContainer::IncludeKeyUnlocked);
      walletOuts.wallet = const_cast<WalletRecord *>(&walletRecord);

      walletOutsVect.push_back(std::move(walletOuts));

    }
  };

  return walletOutsVect;
}

void WalletGreen::prepareInputs(const std::vector<OutputToTransfer>& selectedTransfers, std::vector<CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& mixinResult, uint64_t mixIn, std::vector<InputInfo>& keysInfo) {

  typedef CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry out_entry;

  size_t i = 0;
  for (const OutputToTransfer& input: selectedTransfers) {
    TransactionTypes::InputKeyInfo keyInfo;
    keyInfo.amount = input.out.amount;

    if (mixinResult.size())
    {
      std::sort(mixinResult[i].outs.begin(), mixinResult[i].outs.end(), [] (const out_entry& a, const out_entry& b) { return a.global_amount_index < b.global_amount_index; });
      for (auto& fakeOut: mixinResult[i].outs)
      {

        if (input.out.globalOutputIndex != fakeOut.global_amount_index) {
          TransactionTypes::GlobalOutput globalOutput;
          globalOutput.outputIndex = static_cast<uint32_t>(fakeOut.global_amount_index);
          globalOutput.targetKey = reinterpret_cast<Crypto::PublicKey&>(fakeOut.out_key);
          keyInfo.outputs.push_back(std::move(globalOutput));
          if (keyInfo.outputs.size() >= mixIn)
          {
            break;
          }
        }
      }
    }

    //paste real transaction to the random index
    auto insertIn = std::find_if(keyInfo.outputs.begin(), keyInfo.outputs.end(), [&](const TransactionTypes::GlobalOutput& a) { return a.outputIndex >= input.out.globalOutputIndex; });

    TransactionTypes::GlobalOutput realOutput;
    realOutput.outputIndex = input.out.globalOutputIndex;
    realOutput.targetKey = reinterpret_cast<const Crypto::PublicKey&>(input.out.outputKey);

    auto insertedIn = keyInfo.outputs.insert(insertIn, realOutput);

    keyInfo.realOutput.transactionPublicKey = reinterpret_cast<const Crypto::PublicKey&>(input.out.transactionPublicKey);
    keyInfo.realOutput.transactionIndex = static_cast<size_t>(insertedIn - keyInfo.outputs.begin());
    keyInfo.realOutput.outputInTransaction = input.out.outputInTransaction;

    //Important! outputs in selectedTransfers and in keysInfo must have the same order!
    InputInfo inputInfo;
    inputInfo.keyInfo = std::move(keyInfo);
    inputInfo.walletRecord = input.wallet;
    keysInfo.push_back(std::move(inputInfo));
    ++i;
  }
}

void WalletGreen::prepareTransaction(std::vector<WalletOuts>&& wallets, const std::vector<WalletOrder>& orders, uint64_t fee, uint64_t mixIn, const std::string& extra, uint64_t unlockTimestamp, const DonationSettings& donation, const AccountPublicAddress& changeDestination, PreparedTransaction& preparedTransaction, Crypto::SecretKey& transactionPrivateKey) {

  preparedTransaction.destinations = convertOrdersToTransfers(orders);
  preparedTransaction.neededMoney = countNeededMoney(preparedTransaction.destinations, fee);

  std::vector<OutputToTransfer> selectedTransfers;

  uint64_t dustThreshold = m_currency.getDustThreshold(m_node.getLastKnownBlockHeight());

  uint64_t foundMoney = selectTransfers(preparedTransaction.neededMoney, mixIn == 0, dustThreshold, std::move(wallets), selectedTransfers);

  if (foundMoney < preparedTransaction.neededMoney) {
    throw std::system_error(make_error_code(error::WRONG_AMOUNT), "Not enough money");
  }

  typedef CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount outs_for_amount;
  std::vector<outs_for_amount> mixinResult;

  if (mixIn != 0) {
    requestMixinOuts(selectedTransfers, mixIn, mixinResult);
  }

  std::vector<InputInfo> keysInfo;
  prepareInputs(selectedTransfers, mixinResult, mixIn, keysInfo);

  uint64_t donationAmount = pushDonationTransferIfPossible(donation, foundMoney - preparedTransaction.neededMoney, dustThreshold, preparedTransaction.destinations);
  preparedTransaction.changeAmount = foundMoney - preparedTransaction.neededMoney - donationAmount;

  std::vector<ReceiverAmounts> decomposedOutputs = splitDestinations(preparedTransaction.destinations, dustThreshold, m_currency);
  if (preparedTransaction.changeAmount != 0) {
    WalletTransfer changeTransfer;
    changeTransfer.type = WalletTransferType::CHANGE;
    changeTransfer.address = m_currency.accountAddressAsString(changeDestination);
    changeTransfer.amount = static_cast<int64_t>(preparedTransaction.changeAmount);
    preparedTransaction.destinations.emplace_back(std::move(changeTransfer));

    auto splittedChange = splitAmount(preparedTransaction.changeAmount, changeDestination, dustThreshold);
    decomposedOutputs.emplace_back(std::move(splittedChange));
  }

  preparedTransaction.transaction = makeTransaction(decomposedOutputs, keysInfo, extra, unlockTimestamp, transactionPrivateKey);
}

void WalletGreen::pushBackOutgoingTransfers(size_t transactionIndex, const std::vector<WalletTransfer>& destinations)
{
  for (const WalletTransfer& destination: destinations)
  {
    WalletTransfer d;
    d.type = destination.type;
    d.address = destination.address;
    d.amount = destination.amount;

    m_walletTransfers.emplace_back(transactionIndex, std::move(d));
  }
}

void WalletGreen::pushEvent(const WalletEvent& event)
{
  m_eventsQueue.push(event);
  m_eventOccurred.set();
}

void WalletGreen::removeUnconfirmedTransaction(const Crypto::Hash& transactionHash)
{
  System::RemoteContext<void> context(m_dispatcher, [this, &transactionHash] {
    m_blockchainSynchronizer.removeUnconfirmedTransaction(transactionHash).get();
  });

  context.get();
}

void WalletGreen::requestMixinOuts(const std::vector<OutputToTransfer>& selectedTransfers, uint64_t mixIn, std::vector<CORE_RPC_COMMAND_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& mixinResult) {

  std::vector<uint64_t> amounts;
  for (const OutputToTransfer& outputToTransfer: selectedTransfers)
  {
    amounts.push_back(outputToTransfer.out.amount);
  }

  System::Event requestFinished(m_dispatcher);
  std::error_code mixinError;

  throwIfStopped();

  m_node.getRandomOutsByAmounts(std::move(amounts), mixIn, mixinResult, [&requestFinished, &mixinError, this] (std::error_code ec) {
    mixinError = ec;
    this->m_dispatcher.remoteSpawn(std::bind(asyncRequestCompletion, std::ref(requestFinished)));
  });

  requestFinished.wait();

  checkIfEnoughMixins(mixinResult, mixIn);

  if (mixinError)
  {
    throw std::system_error(mixinError);
  }
}

uint64_t WalletGreen::selectTransfers(uint64_t neededMoney, bool dust, uint64_t dustThreshold, std::vector<WalletOuts>&& wallets, std::vector<OutputToTransfer>& selectedTransfers) {

  uint64_t foundMoney = 0;

  std::vector<WalletOuts> walletOuts = wallets;
  std::default_random_engine randomGenerator(Crypto::rand<std::default_random_engine::result_type>());

  while (foundMoney < neededMoney && !walletOuts.empty()) {
    std::uniform_int_distribution<size_t> walletsDistribution(0, walletOuts.size() - 1);

    size_t walletIndex = walletsDistribution(randomGenerator);
    std::vector<TransactionOutputInformation>& addressOuts = walletOuts[walletIndex].outs;

    assert(addressOuts.size() > 0);
    std::uniform_int_distribution<size_t> outDistribution(0, addressOuts.size() - 1);
    size_t outIndex = outDistribution(randomGenerator);

    TransactionOutputInformation out = addressOuts[outIndex];
    if (out.amount > dustThreshold || dust) {
      if (out.amount <= dustThreshold) {
        dust = false;
      }

      foundMoney += out.amount;

      selectedTransfers.push_back( { std::move(out), walletOuts[walletIndex].wallet } );
    }

    addressOuts.erase(addressOuts.begin() + outIndex);
    if (addressOuts.empty()) {
      walletOuts.erase(walletOuts.begin() + walletIndex);
    }
  }

  if (!dust) {
    return foundMoney;
  }

  for (const auto& addressOuts : walletOuts) {
    auto it = std::find_if(addressOuts.outs.begin(), addressOuts.outs.end(), [dustThreshold] (const TransactionOutputInformation& out) {
      return out.amount <= dustThreshold;
    });

    if (it != addressOuts.outs.end()) {
      foundMoney += it->amount;
      selectedTransfers.push_back({ *it, addressOuts.wallet });
      break;
    }
  }

  return foundMoney;
};

void WalletGreen::sendTransaction(const Transaction& transaction) {
  System::Event completion(m_dispatcher);
  std::error_code ec;

  throwIfStopped();
  m_node.relayTransaction(transaction, [&ec, &completion, this](std::error_code error) {
    ec = error;
    this->m_dispatcher.remoteSpawn(std::bind(asyncRequestCompletion, std::ref(completion)));
  });
  completion.wait();

  if (ec) {
    throw std::system_error(ec);
  }
}

WalletGreen::ReceiverAmounts WalletGreen::splitAmount(uint64_t amount, const AccountPublicAddress& destination, uint64_t dustThreshold) {

  ReceiverAmounts receiverAmounts;

  receiverAmounts.receiver = destination;
  decomposeAmount(amount, dustThreshold, receiverAmounts.amounts);
  return receiverAmounts;
}

std::vector<WalletGreen::ReceiverAmounts> WalletGreen::splitDestinations(const std::vector<WalletTransfer>& destinations, uint64_t dustThreshold, const Currency& currency) {

  std::vector<ReceiverAmounts> decomposedOutputs;
  for (const WalletTransfer& destination: destinations) {
    AccountPublicAddress address;

    if (!currency.parseAccountAddressString(destination.address, address)) {
      throw std::system_error(make_error_code(CryptoNote::error::BAD_ADDRESS));
    }

    decomposedOutputs.push_back(splitAmount(destination.amount, address, dustThreshold));
  }

  return decomposedOutputs;
}

void WalletGreen::startBlockchainSynchronizer()
{
  if (!m_walletsContainer.empty() && !m_blockchainSynchronizerStarted)
  {
    m_blockchainSynchronizer.start();
    m_blockchainSynchronizerStarted = true;
  }
}

void WalletGreen::stopBlockchainSynchronizer()
{
  if (m_blockchainSynchronizerStarted)
  {
    m_blockchainSynchronizer.stop();
    m_blockchainSynchronizerStarted = false;
  }
}

void WalletGreen::synchronizationCompleted(std::error_code result)
{
  m_dispatcher.remoteSpawn([this] () { onSynchronizationCompleted(); } );
}

void WalletGreen::synchronizationProgressUpdated(uint32_t processedBlockCount, uint32_t totalBlockCount)
{
  m_dispatcher.remoteSpawn( [processedBlockCount, totalBlockCount, this] () { onSynchronizationProgressUpdated(processedBlockCount, totalBlockCount); } );
}

void WalletGreen::throwIfNotInitialized() const
{
  if (m_walletState != WalletState::INITIALIZED)
  {
    throw std::system_error(make_error_code(error::NOT_INITIALIZED));
  }
}

void WalletGreen::throwIfStopped() const
{
  if (m_stopped)
  {
    throw std::system_error(make_error_code(error::OPERATION_CANCELLED));
  }
}

void WalletGreen::throwIfTrackingMode() const {
  if (getTrackingMode() == WalletTrackingMode::TRACKING)
  {
    throw std::system_error(make_error_code(error::TRACKING_MODE));
  }
}

// TODO remove
void WalletGreen::transactionDeleteBegin(Crypto::Hash /*transactionHash*/) {
}

// TODO remove
void WalletGreen::transactionDeleteEnd(Crypto::Hash transactionHash) {
}

void WalletGreen::transactionDeleted(ITransfersSubscription* object, const Crypto::Hash& transactionHash) {
  System::EventLock lk(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED) {
    return;
  }

  auto it = m_walletTransactions.get<TransactionIndex>().find(transactionHash);
  if (it == m_walletTransactions.get<TransactionIndex>().end()) {
    return;
  }

  ITransfersContainer* container = &object->getContainer();
  updateBalance(container);
  deleteUnlockTransactionJob(transactionHash);

  bool updated = false;
  m_walletTransactions.get<TransactionIndex>().modify(it, [&updated](WalletTransaction& tx) {
    if (tx.state == WalletTransactionState::CREATED || tx.state == WalletTransactionState::SUCCEEDED) {
      tx.state = WalletTransactionState::CANCELLED;
      updated = true;
    }

    if (tx.blockHeight != WALLET_UNCONFIRMED_TRANSACTION_HEIGHT) {
      tx.blockHeight = WALLET_UNCONFIRMED_TRANSACTION_HEIGHT;
      updated = true;
    }
  });

  if (updated) {
    auto transactionId = getTransactionIndex(transactionHash);
    pushEvent(makeTransactionUpdatedEvent(transactionId));
  }
}

void WalletGreen::transactionUpdated(const TransactionInformation& transactionInfo, const std::vector<ContainerAmounts>& containerAmountsList) {
  System::EventLock lk(m_readyEvent);

  if (m_walletState == WalletState::NOT_INITIALIZED) {
    return;
  }

  bool updated = false;
  bool isNew = false;

  int64_t totalAmount = std::accumulate(containerAmountsList.begin(), containerAmountsList.end(), static_cast<int64_t>(0),
    [](int64_t sum, const ContainerAmounts& containerAmounts) { return sum + containerAmounts.amounts.input + containerAmounts.amounts.output; });

  size_t transactionId;
  auto& hashIndex = m_walletTransactions.get<TransactionIndex>();
  auto it = hashIndex.find(transactionInfo.transactionHash);
  if (it != hashIndex.end()) {
    transactionId = std::distance(m_walletTransactions.get<RandomAccessIndex>().begin(), m_walletTransactions.project<RandomAccessIndex>(it));
    updated |= updateWalletTransactionInfo(transactionId, transactionInfo, totalAmount);
  } else {
    isNew = true;
    transactionId = insertBlockchainTransaction(transactionInfo, totalAmount);
    m_fusionTxsCache.emplace(transactionId, isFusionTransaction(*it));
  }

  if (transactionInfo.blockHeight != WALLET_UNCONFIRMED_TRANSACTION_HEIGHT) {
    // In some cases a transaction can be included to a block but not removed from m_uncommitedTransactions. Fix it
    m_uncommitedTransactions.erase(transactionId);
  }

  // Update cached balance
  for (auto containerAmounts : containerAmountsList) {
    updateBalance(containerAmounts.container);

    if (transactionInfo.blockHeight != WALLET_UNCONFIRMED_TRANSACTION_HEIGHT) {
      uint32_t unlockHeight = std::max(transactionInfo.blockHeight + m_transactionSoftLockTime, static_cast<uint32_t>(transactionInfo.unlockTime));
      insertUnlockTransactionJob(transactionInfo.transactionHash, unlockHeight, containerAmounts.container);
    }
  }

  updated |= updateTransactionTransfers(transactionId, containerAmountsList, -static_cast<int64_t>(transactionInfo.totalAmountIn),
    static_cast<int64_t>(transactionInfo.totalAmountOut));

  if (isNew) {
    CryptoNote::WalletEvent event;
    event.type = CryptoNote::WalletEventType::TRANSACTION_CREATED;
    event.transactionCreated.transactionIndex = transactionId;
    pushEvent(event);
  } else if (updated) {
    pushEvent(makeTransactionUpdatedEvent(transactionId));
  }
}

void WalletGreen::unlockBalances(uint32_t height) {
  auto& index = m_unlockTransactionsJob.get<BlockHeightIndex>();
  auto upper = index.upper_bound(height);

  if (index.begin() != upper) {
    for (auto it = index.begin(); it != upper; ++it) {
      updateBalance(it->container);
    }

    index.erase(index.begin(), upper);

    CryptoNote::WalletEvent event;
    event.type = CryptoNote::WalletEventType::BALANCE_UNLOCKED;
    pushEvent(event);
  }
}

void WalletGreen::unsafeLoad(std::istream& source, const std::string& password)
{
  WalletSerializer s(
    *this,
    m_viewPublicKey,
    m_viewPrivateKey,
    m_actualBalance,
    m_pendingBalance,
    m_walletsContainer,
    m_transfersSynchronizer,
    m_unlockTransactionsJob,
    m_walletTransactions,
    m_walletTransfers,
    m_transactionSoftLockTime,
    m_uncommitedTransactions
  );

  Common::StdInputStream inputStream(source);
  s.load(password, inputStream);

  m_password = password;
  m_blockchainSynchronizer.addObserver(this);
}

void WalletGreen::unsafeSave(std::ostream& destination, bool saveDetails, bool saveCache)
{
  WalletTransactions transactions;
  WalletTransfers transfers;

  if (saveDetails && !saveCache)
  {
    filterOutTransactions(transactions, transfers, [] (const WalletTransaction& tx) {
      return tx.state == WalletTransactionState::CREATED || tx.state == WalletTransactionState::DELETED;
    });
  }
  else if (saveDetails)
  {
    filterOutTransactions(transactions, transfers, [] (const WalletTransaction& tx) {
      return tx.state == WalletTransactionState::DELETED;
    });
  }

  WalletSerializer s(
    *this,
    m_viewPublicKey,
    m_viewPrivateKey,
    m_actualBalance,
    m_pendingBalance,
    m_walletsContainer,
    m_transfersSynchronizer,
    m_unlockTransactionsJob,
    transactions,
    transfers,
    m_transactionSoftLockTime,
    m_uncommitedTransactions
  );

  Common::StdOutputStream output(destination);
  s.save(m_password, output, saveDetails, saveCache);
}

bool WalletGreen::updateAddressTransfers(size_t transactionIndex, size_t firstTransferIndex, const std::string& address, int64_t knownAmount, int64_t targetAmount) {
  assert((knownAmount > 0 && targetAmount > 0) || (knownAmount < 0 && targetAmount < 0) || knownAmount == 0 || targetAmount == 0);

  bool updated = false;

  if (knownAmount != targetAmount)
  {
    if (knownAmount == 0)
    {
      appendTransfer(transactionIndex, firstTransferIndex, address, targetAmount);
      updated = true;
    }
    else if (targetAmount == 0)
    {
      assert(knownAmount != 0);
      updated |= eraseTransfersByAddress(transactionIndex, firstTransferIndex, address, knownAmount > 0);
    }
    else
    {
      updated |= adjustTransfer(transactionIndex, firstTransferIndex, address, targetAmount);
    }
  }

  return updated;
}

void WalletGreen::updateBalance(ITransfersContainer* container) {
  auto it = m_walletsContainer.get<TransfersContainerIndex>().find(container);

  if (it == m_walletsContainer.get<TransfersContainerIndex>().end())
  {
    return;
  }

  uint64_t actual = container->balance(ITransfersContainer::IncludeAllUnlocked);
  uint64_t pending = container->balance(ITransfersContainer::IncludeAllLocked);

  if (it->actualBalance < actual) {
    m_actualBalance += actual - it->actualBalance;
  } else {
    m_actualBalance -= it->actualBalance - actual;
  }

  if (it->pendingBalance < pending) {
    m_pendingBalance += pending - it->pendingBalance;
  } else {
    m_pendingBalance -= it->pendingBalance - pending;
  }

  m_walletsContainer.get<TransfersContainerIndex>().modify(it, [actual, pending] (WalletRecord& wallet) {
    wallet.actualBalance = actual;
    wallet.pendingBalance = pending;
  });
}

void WalletGreen::updateBlockHashesContainerWithViewKey(const Crypto::PublicKey& viewPublicKey)
{
  std::vector<Crypto::Hash> blockHashes = m_transfersSynchronizer.getViewKeyKnownBlocks(m_viewPublicKey);
  m_blockHashesContainer.insert(m_blockHashesContainer.end(), blockHashes.begin(), blockHashes.end());
}

void WalletGreen::updateTransactionStateAndPushEvent(size_t transactionIndex, WalletTransactionState state)
{
  auto it = std::next(m_walletTransactions.get<RandomAccessIndex>().begin(), transactionIndex);

  if (it->state != state)
  {
    m_walletTransactions.get<RandomAccessIndex>().modify(it, [state](WalletTransaction& tx) {
      tx.state = state;
    });

    pushEvent(makeTransactionUpdatedEvent(transactionIndex));
  }
}

bool WalletGreen::updateTransactionTransfers(size_t transactionIndex, const std::vector<ContainerAmounts>& containerAmountsList, int64_t allInputsAmount, int64_t allOutputsAmount) {

  assert(allInputsAmount <= 0);
  assert(allOutputsAmount >= 0);

  bool updated = false;

  auto transfersRange = getTransactionTransfersRange(transactionIndex);
  // Iterators can be invalidated, so the first transfer is addressed by its index
  size_t firstTransferIndex = std::distance(m_walletTransfers.cbegin(), transfersRange.first);

  TransfersMap initialTransfers = getKnownTransfersMap(transactionIndex, firstTransferIndex);

  std::unordered_set<std::string> myInputAddresses;
  std::unordered_set<std::string> myOutputAddresses;
  int64_t myInputsAmount = 0;
  int64_t myOutputsAmount = 0;
  for (auto containerAmount : containerAmountsList) {
    AccountPublicAddress address{ getWalletRecord(containerAmount.container).spendPublicKey, m_viewPublicKey };
    std::string addressString = m_currency.accountAddressAsString(address);

    updated |= updateAddressTransfers(transactionIndex, firstTransferIndex, addressString, initialTransfers[addressString].input, containerAmount.amounts.input);
    updated |= updateAddressTransfers(transactionIndex, firstTransferIndex, addressString, initialTransfers[addressString].output, containerAmount.amounts.output);

    myInputsAmount += containerAmount.amounts.input;
    myOutputsAmount += containerAmount.amounts.output;

    if (containerAmount.amounts.input != 0) {
      myInputAddresses.emplace(addressString);
    }

    if (containerAmount.amounts.output != 0) {
      myOutputAddresses.emplace(addressString);
    }
  }

  assert(myInputsAmount >= allInputsAmount);
  assert(myOutputsAmount <= allOutputsAmount);

  int64_t knownInputsAmount = 0;
  int64_t knownOutputsAmount = 0;
  auto updatedTransfers = getKnownTransfersMap(transactionIndex, firstTransferIndex);
  for (const auto& pair : updatedTransfers) {
    knownInputsAmount += pair.second.input;
    knownOutputsAmount += pair.second.output;
  }

  assert(myInputsAmount >= knownInputsAmount);
  assert(myOutputsAmount <= knownOutputsAmount);

  updated |= updateUnknownTransfers(transactionIndex, firstTransferIndex, myInputAddresses, knownInputsAmount, myInputsAmount, allInputsAmount, false);
  updated |= updateUnknownTransfers(transactionIndex, firstTransferIndex, myOutputAddresses, knownOutputsAmount, myOutputsAmount, allOutputsAmount, true);

  return updated;
}

bool WalletGreen::updateUnknownTransfers(size_t transactionIndex, size_t firstTransferIndex, const std::unordered_set<std::string>& myAddresses, int64_t knownAmount, int64_t myAmount, int64_t totalAmount, bool isOutput) {

  bool updated = false;

  if (std::abs(knownAmount) > std::abs(totalAmount)) {
    updated |= eraseForeignTransfers(transactionIndex, firstTransferIndex, myAddresses, isOutput);
    if (totalAmount == myAmount) {
      updated |= eraseTransfersByAddress(transactionIndex, firstTransferIndex, std::string(), isOutput);
    } else {
      assert(std::abs(totalAmount) > std::abs(myAmount));
      updated |= adjustTransfer(transactionIndex, firstTransferIndex, std::string(), totalAmount - myAmount);
    }
  } else if (knownAmount == totalAmount) {
    updated |= eraseTransfersByAddress(transactionIndex, firstTransferIndex, std::string(), isOutput);
  } else {
    assert(std::abs(totalAmount) > std::abs(knownAmount));
    updated |= adjustTransfer(transactionIndex, firstTransferIndex, std::string(), totalAmount - knownAmount);
  }

  return updated;
}

bool WalletGreen::updateWalletTransactionInfo(size_t transactionIndex, const TransactionInformation& txInfo, int64_t totalAmount) {
  auto& walletTransactions = m_walletTransactions.get<RandomAccessIndex>();
  assert(transactionIndex < walletTransactions.size());
  auto it = std::next(walletTransactions.begin(), transactionIndex);

  bool updated = false;
  bool r = walletTransactions.modify(it, [&txInfo, totalAmount, &updated](WalletTransaction& transaction) {
    if (transaction.blockHeight != txInfo.blockHeight) {
      transaction.blockHeight = txInfo.blockHeight;
      updated = true;
    }

    if (transaction.timestamp != txInfo.timestamp) {
      transaction.timestamp = txInfo.timestamp;
      updated = true;
    }

    bool isSucceeded = transaction.state == WalletTransactionState::SUCCEEDED;
    // If transaction was sent to daemon, it can not have CREATED and FAILED states, its state can be SUCCEEDED, CANCELLED or DELETED
    bool wasSent = transaction.state != WalletTransactionState::CREATED && transaction.state != WalletTransactionState::FAILED;
    bool isConfirmed = transaction.blockHeight != WALLET_UNCONFIRMED_TRANSACTION_HEIGHT;
    if (!isSucceeded && (wasSent || isConfirmed)) {
      //transaction may be deleted first then added again
      transaction.state = WalletTransactionState::SUCCEEDED;
      updated = true;
    }

    if (transaction.totalAmount != totalAmount) {
      transaction.totalAmount = totalAmount;
      updated = true;
    }

    // Fix LegacyWallet error. Some old versions didn't fill extra field
    if (transaction.extra.empty() && !txInfo.extra.empty()) {
      transaction.extra = Common::asString(txInfo.extra);
      updated = true;
    }

    bool isBase = txInfo.totalAmountIn == 0;
    if (transaction.isBase != isBase) {
      transaction.isBase = isBase;
      updated = true;
    }
  });

  assert(r);

  return updated;
}

size_t WalletGreen::validateSaveAndSendTransaction(const ITransactionReader& transaction, const std::vector<WalletTransfer>& destinations, bool isFusion, bool send) {
  BinaryArray serializedTransaction = transaction.getTransactionData();

  if (serializedTransaction.size() > m_upperTransactionSizeLimit)
  {
    throw std::system_error(make_error_code(error::TRANSACTION_SIZE_TOO_BIG));
  }

  Transaction deserializedTransaction;
  if (!fromBinaryArray(deserializedTransaction, serializedTransaction))
  {
    throw std::system_error(make_error_code(error::INTERNAL_WALLET_ERROR), "Failed to deserialize created transaction");
  }

  if (deserializedTransaction.extra.size() > parameters::MAX_TX_EXTRA_SIZE)
  {
    throw std::system_error(make_error_code(error::EXTRA_TOO_LARGE), "Transaction extra size is too large");
  }

  uint64_t fee = transaction.getInputTotalAmount() - transaction.getOutputTotalAmount();                                         
  Crypto::SecretKey transactionPrivateKey;
  transaction.getTransactionSecretKey(transactionPrivateKey);
  size_t transactionIndex = insertOutgoingTransactionAndPushEvent(transaction.getTransactionHash(), fee, transaction.getExtra(), transaction.getUnlockTime(), transactionPrivateKey);
  
  Tools::ScopeExit rollbackTransactionInsertion([this, transactionIndex] {
    updateTransactionStateAndPushEvent(transactionIndex, WalletTransactionState::FAILED);
  });

  m_fusionTxsCache.emplace(transactionIndex, isFusion);
  pushBackOutgoingTransfers(transactionIndex, destinations);

  addUnconfirmedTransaction(transaction);
  
  Tools::ScopeExit rollbackAddingUnconfirmedTransaction([this, &transaction] {
    try {
      removeUnconfirmedTransaction(transaction.getTransactionHash());
    } catch (...) {
      // Ignore any exceptions. If rollback fails then the transaction is stored as unconfirmed and will be deleted after wallet relaunch
      // during transaction pool synchronization
    }
  });

  if (send)
  {
    sendTransaction(deserializedTransaction);
    updateTransactionStateAndPushEvent(transactionIndex, WalletTransactionState::SUCCEEDED);
  }
  else
  {
    assert(m_uncommitedTransactions.count(transactionIndex) == 0);
    m_uncommitedTransactions.emplace(transactionIndex, std::move(deserializedTransaction));
  }

  rollbackAddingUnconfirmedTransaction.cancel();
  rollbackTransactionInsertion.cancel();

  return transactionIndex;
}

void WalletGreen::validateTransactionParameters(const TransactionParameters& transactionParameters)
{
  if (transactionParameters.destinations.empty())
  {
    throw std::system_error(make_error_code(error::ZERO_DESTINATION));
  }

  if (transactionParameters.fee < m_node.getMinimalFee())
  {
    std::string message = "Fee is too small. Fee " + m_currency.formatAmount(transactionParameters.fee) +
      ", minimum fee " + m_currency.formatAmount(m_node.getMinimalFee());
    throw std::system_error(make_error_code(error::FEE_TOO_SMALL), message);
  }

  if (transactionParameters.donation.address.empty() != (transactionParameters.donation.threshold == 0))
  {
    throw std::system_error(make_error_code(error::WRONG_PARAMETERS), "DonationSettings must have both address and threshold parameters filled");
  }

  for (const std::string& sourceAddress : transactionParameters.sourceAddresses)
  {
    if (!CryptoNote::validateAddress(sourceAddress, m_currency))
    {
      throw std::system_error(make_error_code(CryptoNote::error::BAD_ADDRESS));
    }

    if (!isMyAddress(sourceAddress))
    {
      throw std::system_error(make_error_code(error::BAD_ADDRESS), "Source address must belong to current container: " + sourceAddress);
    }
  }

  // validate wallet orders
  for (const CryptoNote::WalletOrder& walletOrder: transactionParameters.destinations)
  {
    if (!CryptoNote::validateAddress(walletOrder.address, m_currency))
    {
      throw std::system_error(make_error_code(CryptoNote::error::BAD_ADDRESS));
    }

    if (walletOrder.amount >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
      throw std::system_error(make_error_code(CryptoNote::error::WRONG_AMOUNT),
        "Order amount must not exceed " + std::to_string(std::numeric_limits<int64_t>::max()));
    }
  }


  if (transactionParameters.changeDestination.empty())
  {
    if (transactionParameters.sourceAddresses.size() > 1)
    {
      throw std::system_error(make_error_code(error::CHANGE_ADDRESS_REQUIRED), "Set change destination address");
    }
    else if (transactionParameters.sourceAddresses.empty() && m_walletsContainer.size() > 1)
    {
      throw std::system_error(make_error_code(error::CHANGE_ADDRESS_REQUIRED), "Set change destination address");
    }
  }
  else
  {
    if (!validateAddress(transactionParameters.changeDestination, m_currency))
    {
      throw std::system_error(make_error_code(error::BAD_ADDRESS), "Wrong change address");
    }

    if (!isMyAddress(transactionParameters.changeDestination))
    {
      throw std::system_error(make_error_code(error::CHANGE_ADDRESS_NOT_FOUND), "Change destination address not found in current container");
    }
  }
}

} //namespace CryptoNote