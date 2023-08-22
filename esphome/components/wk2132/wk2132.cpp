/// @file wk2132.cpp
/// @author @DrCoolzic
/// @brief wk2132 implementation

#include "wk2132.h"

namespace esphome {
namespace wk2132 {

static const char *const TAG = "wk2132";

static const char *const REG_TO_STR_P0[] = {"GENA", "GRST", "GMUT",  "SPAGE", "SCR", "LCR", "FCR",
                                            "SIER", "SIFR", "TFCNT", "RFCNT", "FSR", "LSR", "FDAT"};
static const char *const REG_TO_STR_P1[] = {"GENA", "GRST", "GMUT",  "SPAGE", "BAUD1", "BAUD0", "PRES",
                                            "RFTL", "TFTL", "_INV_", "_INV_", "_INV_", "_INV_"};

// convert an int to binary string
inline std::string i2s(uint8_t val) { return std::bitset<8>(val).to_string(); }
#define I2CS(val) (i2s(val).c_str())

/// @brief Computes the I²C Address to access the component
/// @param base_address the base address of the component as set by the A1 A0 pins
/// @param channel (0-3) the UART channel
/// @param fifo (0-1) if 0 access to internal register, if 1 direct access to fifo
/// @return the i2c address to use
inline uint8_t i2c_address(uint8_t base_address, uint8_t channel, uint8_t fifo) {
  // the address of the device is 0AA1 0CCF (eg: 0001 0000) where:
  // - AA is the address read from A1,A0
  // - CC is the channel number (in practice only 00 or 01)
  // - F is 0 when accessing register one when accessing FIFO
  uint8_t const addr = base_address | channel << 1 | fifo;
  return addr;
}

/// @brief Converts the parity enum to a string
/// @param parity enum
/// @return the string
const char *parity2string(uart::UARTParityOptions parity) {
  using namespace uart;
  switch (parity) {
    case UART_CONFIG_PARITY_NONE:
      return "NONE";
    case UART_CONFIG_PARITY_EVEN:
      return "EVEN";
    case UART_CONFIG_PARITY_ODD:
      return "ODD";
    default:
      return "UNKNOWN";
  }
}

///////////////////////////////////////////////////////////////////////////////
// The WK2132Component methods
///////////////////////////////////////////////////////////////////////////////

// method used in log messages ...
const char *WK2132Component::reg_to_str_(int val) { return page1_ ? REG_TO_STR_P1[val] : REG_TO_STR_P0[val]; }

void WK2132Component::write_wk2132_register_(uint8_t reg_number, uint8_t channel, const uint8_t *buffer, size_t len) {
  address_ = i2c_address(base_address_, channel, 0);  // update the i2c address
  auto error = this->write_register(reg_number, buffer, len);
  if (error == i2c::ERROR_OK) {
    this->status_clear_warning();
    ESP_LOGVV(TAG, "write_wk2132_register_(@%02X %s, ch=%d b=%02X [%s], len=%d): I2C code %d", address_,
              this->reg_to_str_(reg_number), channel, *buffer, I2CS(*buffer), len, (int) error);
  } else {  // error
    this->status_set_warning();
    ESP_LOGE(TAG, "write_wk2132_register_(@%02X %s, ch=%d b=%02X [%s], len=%d): I2C code %d", address_,
             this->reg_to_str_(reg_number), channel, *buffer, I2CS(*buffer), len, (int) error);
  }
}

uint8_t WK2132Component::read_wk2132_register_(uint8_t reg_number, uint8_t channel, uint8_t *buffer, size_t len) {
  address_ = i2c_address(base_address_, channel, 0);  // update the i2c address
  auto error = this->read_register(reg_number, buffer, len);
  if (error == i2c::ERROR_OK) {
    this->status_clear_warning();
    ESP_LOGVV(TAG, "read_wk2132_register_(@%02X %s, ch=%d b=%02X [%s], len=%d): I2C code %d", address_,
              this->reg_to_str_(reg_number), channel, *buffer, I2CS(*buffer), len, (int) error);
  } else {  // error
    this->status_set_warning();
    ESP_LOGE(TAG, "read_wk2132_register_(@%02X %s, ch=%d b=%02X [%s], len=%d): I2C code %d", address_,
             this->reg_to_str_(reg_number), channel, *buffer, I2CS(*buffer), len, (int) error);
  }
  return *buffer;
}

//
// overloaded methods from Component
//
void WK2132Component::setup() {
  this->base_address_ = this->address_;  // TODO should not be necessary done in the ctor
  ESP_LOGCONFIG(TAG, "Setting up WK2132:@%02X with %d UARTs...", get_num_(), base_address_, (int) children_.size());
  // we test communication with device
  read_wk2132_register_(REG_WK2132_GENA, 0, &data_, 1);

  // we setup our children
  for (auto *child : this->children_)
    child->setup_channel_();
}

void WK2132Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Initialization of configuration WK2132:@%02X with %d UARTs completed", get_num_(),
                (int) children_.size());
  ESP_LOGCONFIG(TAG, "  crystal %d", crystal_);
  ESP_LOGCONFIG(TAG, "  test_mode %d", test_mode_);
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with WK2132 failed!");
  }

  for (auto i = 0; i < children_.size(); i++) {
    ESP_LOGCONFIG(TAG, "  UART @%02X:%d...", get_num_(), i);
    ESP_LOGCONFIG(TAG, "    baudrate %d Bd", children_[i]->baud_rate_);
    ESP_LOGCONFIG(TAG, "    data_bits %d", children_[i]->data_bits_);
    ESP_LOGCONFIG(TAG, "    stop_bits %d", children_[i]->stop_bits_);
    ESP_LOGCONFIG(TAG, "    parity %s", parity2string(children_[i]->parity_));
  }
  initialized_ = true;  // ths is the end of the initialization for our component
}

///////////////////////////////////////////////////////////////////////////////
// The WK2132Channel methods
///////////////////////////////////////////////////////////////////////////////

void WK2132Channel::setup_channel_() {
  ESP_LOGCONFIG(TAG, "  Setting up UART @%02X:%d...", parent_->get_num_(), channel_);

  // we first do the global register (common to both channel)

  //  GENA description of global control register:
  //  * -------------------------------------------------------------------------
  //  * |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  //  * -------------------------------------------------------------------------
  //  * |   M1   |   M0   |              RESERVED             |  UT2EN |  UT1EN |
  //  * -------------------------------------------------------------------------
  uint8_t gena;
  parent_->read_wk2132_register_(REG_WK2132_GENA, 0, &gena, 1);
  (this->channel_ == 0) ? gena |= 0x01 : gena |= 0x02;
  parent_->write_wk2132_register_(REG_WK2132_GENA, 0, &gena, 1);

  //  GRST description of global reset register:
  //  * -------------------------------------------------------------------------
  //  * |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  //  * -------------------------------------------------------------------------
  //  * |       RSV       | UT2SLE | UT1SLE |       RSV       | UT2RST | UT1RST |
  //  * -------------------------------------------------------------------------
  // software reset UART channels
  uint8_t grst = 0;
  parent_->read_wk2132_register_(REG_WK2132_GRST, 0, &gena, 1);
  (this->channel_ == 0) ? grst |= 0x01 : grst |= 0x02;
  parent_->write_wk2132_register_(REG_WK2132_GRST, 0, &grst, 1);

  // now we initialize the channel register

  // set page 0
  uint8_t const page = 0;
  parent_->page1_ = false;
  parent_->write_wk2132_register_(REG_WK2132_SPAGE, channel_, &page, 1);

  // FCR description of UART FIFO control register:
  // -------------------------------------------------------------------------
  // |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  // -------------------------------------------------------------------------
  // |      TFTRIG     |      RFTRIG     |  TFEN  |  RFEN  |  TFRST |  RFRST |
  // -------------------------------------------------------------------------
  uint8_t const fsr = 0x0F;  // 0000 1111 reset fifo and enable the two fifo ...
  parent_->write_wk2132_register_(REG_WK2132_FCR, channel_, &fsr, 1);

  // SCR description of UART control register:
  //  -------------------------------------------------------------------------
  //  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  //  -------------------------------------------------------------------------
  //  |                     RSV                    | SLEEPEN|  TXEN  |  RXEN  |
  //  -------------------------------------------------------------------------
  uint8_t const scr = 0x3;  // 0000 0011 enable receive and transmit
  parent_->write_wk2132_register_(REG_WK2132_SCR, channel_, &scr, 1);

  set_baudrate_();
  set_line_param_();
}

void WK2132Channel::set_baudrate_() {
  uint16_t const val_int = parent_->crystal_ / (baud_rate_ * 16) - 1;
  uint16_t val_dec = (parent_->crystal_ % (baud_rate_ * 16)) / (baud_rate_ * 16);
  uint8_t const baud_high = (uint8_t) (val_int >> 8);
  uint8_t const baud_low = (uint8_t) (val_int & 0xFF);
  while (val_dec > 0x0A)
    val_dec /= 0x0A;
  uint8_t const baud_dec = (uint8_t) (val_dec);

  uint8_t page = 1;  // switch to page 1
  parent_->write_wk2132_register_(REG_WK2132_SPAGE, channel_, &page, 1);
  parent_->page1_ = true;
  parent_->write_wk2132_register_(REG_WK2132_BRH, channel_, &baud_high, 1);
  parent_->write_wk2132_register_(REG_WK2132_BRL, channel_, &baud_low, 1);
  parent_->write_wk2132_register_(REG_WK2132_BRD, channel_, &baud_dec, 1);
  page = 0;  // switch back to page 0
  parent_->write_wk2132_register_(REG_WK2132_SPAGE, channel_, &page, 1);
  parent_->page1_ = false;

  ESP_LOGCONFIG(TAG, "  Crystal=%d baudrate=%d => registers [%d %d %d]", parent_->crystal_, baud_rate_, baud_high,
                baud_low, baud_dec);
}

void WK2132Channel::set_line_param_() {
  data_bits_ = 8;  // always 8 for WK2132
  uint8_t lcr;
  parent_->read_wk2132_register_(REG_WK2132_LCR, channel_, &lcr, 1);
  // LCR description of line configuration register:
  //  -------------------------------------------------------------------------
  //  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  //  -------------------------------------------------------------------------
  //  |        RSV      |  BREAK |  IREN  |  PAEN  |      PAM        |  STPL  |
  //  -------------------------------------------------------------------------
  lcr &= 0xF0;  // Clear the lower 4 bit of LCR
  if (this->stop_bits_ == 2)
    lcr |= 0x01;  // 0001

  switch (this->parity_) {              // parity selection settings
    case uart::UART_CONFIG_PARITY_ODD:  // odd parity
      lcr |= 0x5 << 1;                  // 101x
      break;
    case uart::UART_CONFIG_PARITY_EVEN:  // even parity
      lcr |= 0x6 << 1;                   // 110x
      break;
    default:
      break;  // no parity 000x
  }
  parent_->write_wk2132_register_(REG_WK2132_LCR, channel_, &lcr, 1);
  ESP_LOGCONFIG(TAG, "  line config: %d data_bits, %d stop_bits, parity %s register [%s]", data_bits_, stop_bits_,
                parity2string(parity_), I2CS(lcr));
}

size_t WK2132Channel::tx_in_fifo_() {
  // FSR description of line configuration register:
  //  * -------------------------------------------------------------------------
  //  * |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
  //  * -------------------------------------------------------------------------
  //  * |  RFOE  |  RFBI  |  RFFE  |  RFPE  |  RDAT  |  TDAT  |  TFULL |  TBUSY |
  //  * -------------------------------------------------------------------------
  uint8_t const fsr = this->parent_->read_wk2132_register_(REG_WK2132_FSR, channel_, &data_, 1);
  uint8_t const tfcnt = this->parent_->read_wk2132_register_(REG_WK2132_TFCNT, channel_, &data_, 1);
  ESP_LOGVV(TAG, "tx_in_fifo=%d FSR=%s", tfcnt, I2CS(fsr));
  return tfcnt;
}

size_t WK2132Channel::rx_in_fifo_() {
  uint8_t available = 0;
  uint8_t const fsr = this->parent_->read_wk2132_register_(REG_WK2132_FSR, channel_, &data_, 1);
  if (fsr & 0x8)
    available = this->parent_->read_wk2132_register_(REG_WK2132_RFCNT, channel_, &data_, 1);
  if (!peek_buffer_.empty)
    available++;
  if (available > this->fifo_size_())  // no more than what is set in the fifo_size
    available = this->fifo_size_();

  ESP_LOGVV(TAG, "rx_in_fifo %d (byte in peek_buffer: %s) FSR=%s", available, peek_buffer_.empty ? "no" : "yes",
            I2CS(fsr));
  return available;
}

bool WK2132Channel::read_data_(uint8_t *buffer, size_t len) {
  parent_->address_ = i2c_address(parent_->base_address_, channel_, 1);  // set fifo flag
  // With the WK2132 we need to read data directly from the fifo buffer without passing through a register
  // note: that theoretically it should be possible to read through the REG_WK2132_FDA register
  // but beware that it does not seems to work !
  auto error = parent_->read(buffer, len);
  if (error == i2c::ERROR_OK) {
    parent_->status_clear_warning();
    if (parent_->test_mode_.test(1) && parent_->initialized_)  // test sniff (bit 1)
      ESP_LOGI(TAG, "snif: received %d chars %02X... on UART @%02X channel %d", len, *buffer, parent_->base_address_,
               channel_);
    ESP_LOGV(TAG, "read_data(ch=%d buffer[0]=%02X [%s], len=%d): I2C code %d", channel_, *buffer, I2CS(*buffer), len,
             (int) error);
    return true;
  } else {  // error
    parent_->status_set_warning();
    ESP_LOGE(TAG, "read_data(ch=%d buffer[0]=%02X [%s], len=%d): I2C code %d", channel_, *buffer, I2CS(*buffer), len,
             (int) error);
    return false;
  }
}

bool WK2132Channel::write_data_(const uint8_t *buffer, size_t len) {
  parent_->address_ = i2c_address(parent_->base_address_, this->channel_, 1);  // set fifo flag

  // With the WK2132 we need to write to the fifo buffer without passing through a register
  // note: that theoretically it should be possible to write through the REG_WK2132_FDA register
  // but beware that it does not seems to work !
  auto error = parent_->write(buffer, len);
  if (error == i2c::ERROR_OK) {
    parent_->status_clear_warning();
    if (parent_->test_mode_.test(1) && parent_->initialized_)  // test sniff (bit 1)
      ESP_LOGI(TAG, "sniff: sent %d chars %02X... on UART @%02X channel %d", len, *buffer, parent_->base_address_,
               channel_);
    ESP_LOGV(TAG, "write_data(ch=%d buffer[0]=%02X [%s], len=%d): I2C code %d", channel_, *buffer, I2CS(*buffer), len,
             (int) error);
    return true;
  } else {  // error
    parent_->status_set_warning();
    ESP_LOGE(TAG, "write_data(ch=%d buffer[0]=%02X [%s], len=%d): I2C code %d", channel_, *buffer, I2CS(*buffer), len,
             (int) error);
    return false;
  }
}

bool WK2132Channel::read_array(uint8_t *buffer, size_t len) {
  if (len > fifo_size_()) {
    ESP_LOGE(TAG, "Read buffer invalid call: requested %d bytes max size %d ...", len, fifo_size_());
    return false;
  }

  if (!peek_buffer_.empty) {  // test peek buffer
    *buffer++ = peek_buffer_.data;
    peek_buffer_.empty = true;
    if (len-- == 1)
      return true;
  }

  bool status = true;
  uint32_t const start_time = millis();
  // in safe mode we check that we have received the requested characters
  while (safe_ && this->rx_in_fifo_() < len) {
    if (millis() - start_time > 100) {  // we wait as much as 100 ms
      ESP_LOGE(TAG, "Read buffer underrun: requested %d bytes only received %d ...", len, this->rx_in_fifo_());
      len = this->rx_in_fifo_();  // set length to what is in the buffer
      status = false;
      break;
    }
    yield();  // reschedule our thread at end of queue
  }
  read_data_(buffer, len);
  return status;
}

int WK2132Channel::available() {
  auto available = this->rx_in_fifo_();
  if (parent_->test_mode_.test(1) && parent_->initialized_)  // test sniff (bit 1)
    ESP_LOGI(TAG, "sniff: %d chars available in UART@%02X channel %d", available, parent_->base_address_, channel_);
  return available;
}

bool WK2132Channel::peek_byte(uint8_t *buffer) {
  if (safe_ && peek_buffer_.empty && this->available() == 0)
    return false;
  if (peek_buffer_.empty) {
    peek_buffer_.empty = false;
    read_data_(&peek_buffer_.data, 1);
  }
  *buffer = peek_buffer_.data;
  return true;
}

void WK2132Channel::write_array(const uint8_t *buffer, size_t len) {
  if (len > fifo_size_()) {
    ESP_LOGE(TAG, "Write buffer invalid call: requested %d bytes max size %d ...", len, fifo_size_());
    len = fifo_size_();
  }
  if (safe_ && (len > (fifo_size_() - tx_in_fifo_()))) {
    len = fifo_size_() - tx_in_fifo_();  // send as much as possible
    ESP_LOGE(TAG, "Write buffer overrun: can only send %d bytes ...", len);
  }
  write_data_(buffer, len);
}

void WK2132Channel::flush() {
  uint32_t const start_time = millis();
  while (tx_in_fifo_()) {  // wait until buffer empty
    if (millis() - start_time > 100) {
      ESP_LOGE(TAG, "Flush timed out: still %d bytes not sent...", fifo_size_() - tx_in_fifo_());
      return;
    }
    yield();  // reschedule thread to avoid blocking
  }
}

///////////////////////////////////////////////////////////////////////////////
/// AUTOTEST FUNCTIONS BELOW
///////////////////////////////////////////////////////////////////////////////
#define AUTOTEST_COMPONENT
#ifdef AUTOTEST_COMPONENT

class Increment {  // A "Functor" (A class object that acts like a method with state!)
 public:
  Increment() : i_(0) {}
  uint8_t operator()() { return i_++; }

 private:
  uint8_t i_;
};

void print_buffer(std::vector<uint8_t> buffer) {
  // quick and ugly hex converter to display buffer in hex format
  char hex_buffer[80];
  hex_buffer[50] = 0;
  for (size_t i = 0; i < buffer.size(); i++) {
    snprintf(&hex_buffer[3 * (i % 16)], sizeof(hex_buffer), "%02X ", buffer[i]);
    if (i % 16 == 15)
      ESP_LOGI(TAG, "   %s", hex_buffer);
  }
  if (buffer.size() % 16) {
    // null terminate if incomplete line
    hex_buffer[3 * (buffer.size() % 16) + 2] = 0;
    ESP_LOGI(TAG, "   %s", hex_buffer);
  }
}

/// @brief test the write_array method
void WK2132Channel::uart_send_test_(char *preamble) {
  auto start_exec = millis();
  uint8_t const to_send = fifo_size_() - tx_in_fifo_();
  uint8_t const to_flush = tx_in_fifo_();  // byte in buffer before execution
  this->flush();                           // we wait until they are gone
  uint8_t const remains = tx_in_fifo_();   // remaining bytes if not null => flush timeout

  if (to_send > 0) {
    std::vector<uint8_t> output_buffer(to_send);
    generate(output_buffer.begin(), output_buffer.end(), Increment());  // fill with incrementing number
    output_buffer[0] = to_send;                     // we send as the first byte the length of the buffer
    this->write_array(&output_buffer[0], to_send);  // we send the buffer
    ESP_LOGI(TAG, "%s pre flushing %d, remains %d => sending %d bytes - exec time %d ms ...", preamble, to_flush,
             remains, to_send, millis() - start_exec);
  }
}

/// @brief test read_array method
void WK2132Channel::uart_receive_test_(char *preamble, bool print_buf) {
  auto start_exec = millis();
  bool status = true;
  uint8_t const to_read = this->available();
  ESP_LOGI(TAG, "%s => %d bytes received status %s - exec time %d ms ...", preamble, to_read, status ? "OK" : "ERROR",
           millis() - start_exec);
  if (to_read > 0) {
    std::vector<uint8_t> buffer(to_read);
    status = read_array(&buffer[0], to_read);
    if (print_buf)
      print_buffer(buffer);
  }
}

void WK2132Component::loop() {
  //
  // This loop is used only if the wk2132 component is in test mode otherwise we return immediately
  //
  if (!initialized_ || test_mode_.none())
    return;
  static uint16_t loop_calls = 0;

  if (test_mode_.test(0)) {  // test loop mode (bit 0)
    static uint32_t loop_time = 0;
    loop_time = millis();
    ESP_LOGI(TAG, "loop %d : %d ms since last call ...", loop_calls++, millis() - loop_time);
    static uint8_t loop_count = 0;
    char preamble[64];
    if (loop_count++ > 3)
      test_mode_.reset(0);  // we reset the loop bit
    for (size_t i = 0; i < children_.size(); i++) {
      snprintf(preamble, sizeof(preamble), "WK2132_@%02X_Ch_%d", get_num_(), i);
      children_[i]->uart_send_test_(preamble);
      children_[i]->uart_receive_test_(preamble);
    }
    ESP_LOGI(TAG, "loop execution time %d ms...", millis() - loop_time);
  }

  if (test_mode_.test(2)) {  // test echo mode (bit 2)
    for (auto *child : this->children_) {
      uint8_t data;
      if (child->available()) {
        child->read_byte(&data);
        ESP_LOGI(TAG, "echo received one char %0X", data);
        child->write_byte(data);
      }
    }
  }
}

#else
void WK2132Component::loop() {}
#endif

}  // namespace wk2132
}  // namespace esphome
