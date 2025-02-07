/// @file wk2132_i2c.h
/// @author DrCoolZic
/// @brief  wk2132 classes declaration

#pragma once
#include <bitset>
#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/uart/uart.h"

/// when TEST_COMPONENT is define we include some auto-test methods. Used to test the software during wk2132 development
/// but can also be used in situ to test if the component is working correctly.
#define TEST_COMPONENT

namespace esphome {
namespace wk2132_i2c {

/// @brief XFER_MAX_SIZE defines the max number of bytes we allow during one transfer. By default I2cBus defines a
/// maximum transfer of 128 bytes but this can be changed by defining the macro I2C_BUFFER_LENGTH.
/// @bug At the time of writing (Nov 2023) there is a bug in declaration of the i2c::I2CDevice::write() method.
/// There is also a bug in the Arduino framework in the declaration of the TwoWire::requestFrom() method.
/// These two bugs limit the XFER_MAX_SIZE to 255.
#if (I2C_BUFFER_LENGTH > 255) && defined(USE_ESP32_FRAMEWORK_ARDUINO)
constexpr size_t XFER_MAX_SIZE = 255;
#else
constexpr size_t XFER_MAX_SIZE = I2C_BUFFER_LENGTH;
#endif

/// @brief size of the internal WK2132 FIFO
constexpr size_t FIFO_SIZE = 256;

/// @brief size of the ring buffer
/// @details We set the size of ring buffer to XFER_MAX_SIZE
constexpr size_t RING_BUFFER_SIZE = XFER_MAX_SIZE;
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief This is an helper class that provides a simple ring buffers that works as a FIFO
/// @details This ring buffer is used to buffer the bytes received in the FIFO of the I2C device. The best way to read
/// characters from the line, is to first check how many bytes were received and then read them all at once.
/// Unfortunately on almost all the code I have reviewed the characters are read one by one in a while loop: check if
/// bytes are available then read the next one until no more byte available. This is pretty inefficient for two reasons:
/// - Fist you need to perform a test for each byte to read
/// - and second you call the read byte method for each character.
/// Assuming you need to read 100 bytes that results into 200 calls instead of 100. Where if you had followed the good
/// practice this could be done in 2 calls (one to find the number of bytes available plus one to read all the bytes! If
/// the registers you read are located on the micro-controller this is not too bad even if it roughly double the process
/// time. But when the registers to check are located on the WK2132 device the performance can get pretty bad as each
/// access to a register requires several cycles on the slow I2C bus. To fix this problem we could ask the users to
/// rewrite their code to follow the good practice but this is not obviously not realistic.
/// @n Therefore the solution I have implemented is to store the bytes received in the FIFO on a local ring buffer.
/// The carefully crafted algorithm used reduces drastically the number of transactions on the i2c bus but more
/// importantly it improve the performance as if the remote registers were located on the micro-controller
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
template<typename T, size_t SIZE> class RingBuffer {
 public:
  /// @brief pushes an item at the tail of the fifo
  /// @param item item to push
  /// @return true if item has been pushed, false il item was not pushed (buffer full)
  bool push(const T item) {
    if (is_full())
      return false;
    this->rb_[this->head_] = item;
    this->head_ = (this->head_ + 1) % SIZE;
    this->count_++;
    return true;
  }

  /// @brief return and remove the item at head of the fifo
  /// @param item item read
  /// @return true if item has been retrieved, false il no item was found (buffer empty)
  bool pop(T &item) {
    if (is_empty())
      return false;
    item = this->rb_[this->tail_];
    this->tail_ = (this->tail_ + 1) % SIZE;
    this->count_--;
    return true;
  }

  /// @brief return the value of the item at fifo's head without removing it
  /// @param item pointer to item to return
  /// @return true if item has been retrieved, false il no item was found (buffer empty)
  bool peek(T &item) {
    if (is_empty())
      return false;
    item = this->rb_[this->tail_];
    return true;
  }

  /// @brief test is the Ring Buffer is empty ?
  /// @return true if empty
  bool is_empty() { return (this->count_ == 0); }

  /// @brief test is the ring buffer is full ?
  /// @return true if full
  bool is_full() { return (this->count_ == SIZE); }

  /// @brief return the number of item in the ring buffer
  /// @return the number of items
  size_t count() { return this->count_; }

  /// @brief returns the number of free positions in the buffer
  /// @return how many items can be added
  size_t free() { return SIZE - this->count_; }

  /// @brief clear the buffer content
  void clear() { this->head_ = this->tail_ = this->count_ = 0; }

 private:
  std::array<T, SIZE> rb_{0};  ///< the ring buffer
  int tail_{0};                ///< position of the next element to read
  int head_{0};                ///< position of the next element to write
  size_t count_{0};            ///< count number of element in the buffer
};

class WK2132Component;  // forward declaration

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief This helper class creates objects that act as proxies to WK2132 register
/// @details This class is similar to thr i2c::I2CRegister class. The reason for this class to exist is due to the
/// fact that the WK2132 uses a very unusual addressing mechanism:
/// - On a *standard* I2C device the **logical_register_address** is usually combined with the **channel number** to
///   generate an **i2c_register_address**. All accesses to the device are done through a unique address on the bus.
/// - On the WK2132 **i2c_register_address** is always equal to **logical_register_address**. But the address used to
///   access the device on the bus changes according to the channel or the FIFO. Therefore we use a **base_address** to
///   access the global registers, a different addresses to access channel 1 or channel 2 registers, and yet other
///   addresses to access the device's FIFO. For that reason we need to store the register address as well as the
///   channel number for that register.
/// @n For example If the base address is 0x70 we access channel 1 registers at 0x70, channel 1 FIFO at 0x71, channel 2
/// registers at 0x72, Channel 2 FIFO at 0x73.
/// @n typical usage of WK2132Register:
/// @code
///   WK2132Register reg_1 {&WK2132Component_1, ADDR_REGISTER_1, CHANNEL_NUM}  // declaration
///   reg_1 |= 0x01; // set bit 0 of the wk2132 register
///   reg_1 &= ~0x01; // reset bit 0 of the wk2132 register
///   reg_1 = 10; // Set the value of wk2132 register
///   uint val = reg_1; // get the value of wk2132 register
/// @endcode
/// @note The Wk2132Component class provides a WK2132Component::component_reg() method that call the WK2132Register()
/// constructor with the right parameters. The WK2132Channel class provides the WK2132Channel::channel_reg() method for
/// the same reason.
class WK2132Register {
 public:
  /// @brief overloads the = operator. This is used to set a value into the wk2132 register
  /// @param value to be set
  /// @return this object
  WK2132Register &operator=(uint8_t value);

  /// @brief overloads the compound &= operator. This is often used to reset bits in the wk2132 register
  /// @param value performs an & operation with value and store the result
  /// @return this object
  WK2132Register &operator&=(uint8_t value);

  /// @brief overloads the compound |= operator. This is often used to set bits in the wk2132 register
  /// @param value performs an | operation with value and store the result
  /// @return this object
  WK2132Register &operator|=(uint8_t value);

  /// @brief cast operator return the content of the wk2132 register
  operator uint8_t() const { return get(); }

  /// @brief returns the value of the wk2132 register
  /// @return the value of the wk2132 register
  uint8_t get() const;

  /// @brief sets the wk2132 register value
  /// @param value to set
  void set(uint8_t value);

 protected:
  friend class WK2132Component;
  friend class WK2132Channel;

  /// @brief protected constructor. Only friends can create an WK2132Register
  /// @param parent our creator
  /// @param reg address of the i2c register
  /// @param channel the channel of this register
  WK2132Register(WK2132Component *parent, uint8_t reg, uint8_t channel)
      : parent_(parent), register_(reg), channel_(channel) {}

  WK2132Component *parent_;  ///< pointer to our parent (aggregation)
  uint8_t register_;         ///< the address of the register
  uint8_t channel_;          ///< the channel of this register
};

////////////////////////////////////////////////////////////////////////////////////////
/// Definition of the WK2132 registers
////////////////////////////////////////////////////////////////////////////////////////

/// @defgroup wk2132_gr_ WK2132 Global Registers
/// This topic groups all **Global Registers**: these registers are global to the
/// the WK2132 chip (i.e. independent of the UART channel used)
/// @note only registers and parameters used have been documented
/// @{

/// @brief Global Control Register
/// @details @code
///  -------------------------------------------------------------------------
///  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
///  -------------------------------------------------------------------------
///  |   M0   |   M1   |                RSV                |  C2EN  |  C1EN  |
///  -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_GENA = 0x00;
/// @brief Channel 2 enable clock (0: disable, 1: enable)
constexpr uint8_t GENA_C2EN = 1 << 1;
/// @brief Channel 1 enable clock (0: disable, 1: enable)
constexpr uint8_t GENA_C1EN = 1 << 0;

/// @brief Global Reset Register
/// @details @code
///  -------------------------------------------------------------------------
///  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
///  -------------------------------------------------------------------------
///  |       RSV       | C2SLEEP| C1SLEEP|       RSV       |  C2RST |  C1RST |
///  -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_GRST = 0x01;
/// @brief Channel 2 soft reset (0: not reset, 1: reset)
constexpr uint8_t GRST_C2RST = 1 << 1;
/// @brief Channel 1 soft reset (0: not reset, 1: reset)
constexpr uint8_t GRST_C1RST = 1 << 0;

/// @brief Global Master channel control register (not used)
constexpr uint8_t REG_WK2132_GMUT = 0x02;

/// @brief Global Page register
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                             RSV                              |  PAGE  |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_SPAGE = 0x03;

/// Global interrupt register (not used)
constexpr uint8_t REG_WK2132_GIR = 0x10;

/// Global interrupt flag register (not used)
constexpr uint8_t REG_WK2132_GIFR = 0x11;

/// @}
/// @defgroup wk2132_cr_ WK2132 Channel Registers
/// This topic groups all the **Channel Registers**: these registers are specific
/// to the a specific channel i.e. each channel has its own set of registers
/// @note only registers and parameters used have been documented
/// @{

/// @defgroup cr_p0 Channel registers for SPAGE=0
/// The channel registers are further splitted into two groups.
/// This first group is defined when the Global register REG_WK2132_SPAGE is 0
/// @{

/// @brief Serial Control Register
/// @details @code
///  -------------------------------------------------------------------------
///  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
///  -------------------------------------------------------------------------
///  |                     RSV                    | SLEEPEN|  TXEN  |  RXEN  |
///  -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_SCR = 0x04;
/// @brief transmission control (0: enable, 1: disable)
constexpr uint8_t SCR_TXEN = 1 << 1;
/// @brief receiving control (0: enable, 1: disable)
constexpr uint8_t SCR_RXEN = 1 << 0;

/// @brief Line Configuration Register:
/// @details @code
///  -------------------------------------------------------------------------
///  |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
///  -------------------------------------------------------------------------
///  |        RSV      |  BREAK |  IREN  |  PAEN  |      PARITY     |  STPL  |
///  -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_LCR = 0x05;
/// @brief Parity enable (0: no check, 1: check)
constexpr uint8_t LCR_PAEN = 1 << 3;
/// @brief Parity force 0
constexpr uint8_t LCR_PAR_0 = 00 << 1;
/// @brief Parity odd
constexpr uint8_t LCR_PAR_ODD = 01 << 1;
/// @brief Parity even
constexpr uint8_t LCR_PAR_EVEN = 2 << 1;
/// @brief Parity force 1
constexpr uint8_t LCR_PAR_1 = 3 << 1;
/// @brief Stop length (0: 1 bit, 1: 2 bits)
constexpr uint8_t LCR_STPL = 1 << 0;

/// @brief FIFO Control Register
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |      TFTRIG     |      RFTRIG     |  TFEN  |  RFEN  |  TFRST |  RFRST |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_FCR = 0x06;
/// @brief Transmitter FIFO enable
constexpr uint8_t FCR_TFEN = 1 << 3;
/// @brief Receiver FIFO enable
constexpr uint8_t FCR_RFEN = 1 << 2;
/// @brief Transmitter FIFO reset
constexpr uint8_t FCR_TFRST = 1 << 3;
/// @brief Receiver FIFO reset
constexpr uint8_t FCR_RFRST = 1 << 3;

/// @brief Serial Interrupt Enable Register (not used)
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |FERR_IEN|            RSV           |TEMPTY_E|TTRIG_IE|RXOVT_EN|RFTRIG_E|
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_SIER = 0x07;

/// @brief Serial Interrupt Flag Register (not used)
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |      TFTRIG     |      RFTRIG     |  TFEN  |  RFEN  |  TFRST |  RFRST |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_SIFR = 0x08;

/// @brief Transmitter FIFO Count
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                  NUMBER OF DATA IN TRANSMITTER FIFO                   |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_TFCNT = 0x09;

/// @brief Receiver FIFO count
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                    NUMBER OF DATA IN RECEIVER FIFO                    |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_RFCNT = 0x0A;

/// @brief FIFO Status Register
/// @details @code
/// * -------------------------------------------------------------------------
/// * |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// * -------------------------------------------------------------------------
/// * |  RFOE  |  RFLB  |  RFFE  |  RFPE   | RFEMPT | TFEMPT | TFFULL |  TBUSY |
/// * -------------------------------------------------------------------------
/// @endcode
/// @warning The received buffer can hold 256 bytes. However, as the RFCNT reg
/// is 8 bits, in this case the value 256 is reported as 0 ! Therefore the RFCNT
/// count can be zero when there is 0 byte **or** 256 bytes in the buffer. If we
/// have RXDAT = 1 and RFCNT = 0 it should be interpreted as 256 bytes in the FIFO.
/// @note Note that in case of overflow the RFOE goes to one **but** as soon as you read
/// the FSR this bit is cleared. Therefore Overflow can be read only once even if
/// still in overflow.
/// @n The same remark applies to the transmit buffer but here we have to check the
/// TFULL flag. So if TFULL is set and TFCNT is 0 this should be interpreted as 256
constexpr uint8_t REG_WK2132_FSR = 0x0B;
/// @brief Receiver FIFO Overflow Error (0: no OE, 1: OE)
constexpr uint8_t FSR_RFOE = 1 << 7;
/// @brief Receiver FIFO Line Break (0: no LB, 1: LB)
constexpr uint8_t FSR_RFLB = 1 << 6;
/// @brief Receiver FIFO Frame Error (0: no FE, 1: FE)
constexpr uint8_t FSR_RFFE = 1 << 5;
/// @brief Receiver Parity Error (0: no PE, 1: PE)
constexpr uint8_t FSR_RFPE = 1 << 4;
/// @brief Receiver FIFO empty (0: empty, 1: not empty)
constexpr uint8_t FSR_RFEMPTY = 1 << 3;
/// @brief Transmitter FIFO Empty (0: empty, 1: not empty)
constexpr uint8_t FSR_TFEMPTY = 1 << 2;
/// @brief Transmitter FIFO full (0: not full, 1: full)
constexpr uint8_t FSR_TFFULL = 1 << 1;
/// @brief Transmitter busy (0 transmitter empty, 1: transmitter busy sending)
constexpr uint8_t FSR_TBUSY = 1 << 0;

/// @brief Line Status Register (not used - using FIFO)
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                 RSV               |  OVLE  |  BRKE  | FRAMEE |  PAR_E |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_LSR = 0x0C;

/// @brief FIFO Data Register (not used - does not seems to work)
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                        DATA_READ or DATA_TO_WRITE                     |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_FDAT = 0x0D;

/// @}
/// @defgroup cr_p1 Channel registers for SPAGE=1
/// The channel registers are further splitted into two groups.
/// This second group is defined when the Global register REG_WK2132_SPAGE is 1
/// @{

/// @brief Baud rate configuration register: high byte
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                      High byte of the baud rate                       |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_BRH = 0x04;

/// @brief Baud rate configuration register: low byte
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                       Low byte of the baud rate                       |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_BRL = 0x05;

/// @brief Baud rate configuration register decimal part
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                      decimal part of the baud rate                    |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_BRD = 0x06;

/// @brief Receive FIFO Interrupt trigger configuration (not used)
/// @details @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                      Receive FIFO contact control                     |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_RFI = 0x07;

/// @brief Transmit FIFO interrupt trigger configuration (not used)
/// @code
/// -------------------------------------------------------------------------
/// |   b7   |   b6   |   b5   |   b4   |   b3   |   b2   |   b1   |   b0   |
/// -------------------------------------------------------------------------
/// |                       Send FIFO contact control                       |
/// -------------------------------------------------------------------------
/// @endcode
constexpr uint8_t REG_WK2132_TFI = 0x08;

/// @}
/// @}

class WK2132Channel;  // forward declaration

////////////////////////////////////////////////////////////////////////////////////
/// @brief The WK2132Component class stores the information global to the WK2132 component
/// and provides methods to set/access this information.
/// @details For more information please refer to @ref WK2132Component_
////////////////////////////////////////////////////////////////////////////////////
class WK2132Component : public Component, public i2c::I2CDevice {
 public:
  /// @brief store crystal frequency
  /// @param crystal frequency
  void set_crystal(uint32_t crystal) { this->crystal_ = crystal; }

  /// @brief store the component in test mode only use for debug purpose
  /// @param test_mode 0=normal other means component in test mode
  void set_test_mode(int test_mode) { this->test_mode_ = test_mode; }

  /// @brief store the name for the component
  /// @param name the name as defined by the python code generator
  void set_name(std::string name) { this->name_ = std::move(name); }

  /// @brief Get the name of the component
  /// @return the name
  const char *get_name() { return this->name_.c_str(); }

  /// @brief call the WK2132Register constructor to create the proxy
  /// @param a_register address of the register
  /// @return an WK2132Register proxy to the register at a_address
  WK2132Register component_reg(uint8_t a_register) { return {this, a_register, 0}; }

  //
  //  override virtual Component methods
  //

  void setup() override;
  void dump_config() override;
  void loop() override;

  /// @brief Set the priority of the component
  /// @return the priority
  /// @details The priority is set just a bit  below setup_priority::BUS because we use
  /// the i2c bus (which has a priority of BUS) to communicate and the WK2132
  /// will be used by our client as if ir was a bus.
  float get_setup_priority() const override { return setup_priority::BUS - 0.1F; }

 protected:
  friend class WK2132Channel;
  friend class WK2132Register;

  uint32_t crystal_;                         ///< crystal value;
  uint8_t base_address_;                     ///< base address of I2C device
  int test_mode_;                            ///< test mode value (0 -> no tests)
  bool page1_{false};                        ///< set to true when in "page1 mode"
  std::vector<WK2132Channel *> children_{};  ///< the list of WK2132Channel UART children
  std::string name_;                         ///< name of entity
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief The WK2132Channel class is used to implement all the virtual methods of the ESPHome uart::UARTComponent
/// class.
/// @details For more information see @ref WK2132Channel_
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class WK2132Channel : public uart::UARTComponent {
 public:
  /// @brief We belong to the parent WK2132Component
  /// @param parent pointer to the component we belongs to
  void set_parent(WK2132Component *parent) {
    this->parent_ = parent;
    this->parent_->children_.push_back(this);  // add ourself to the list (vector)
  }

  /// @brief Sets the channel number
  /// @param channel number
  void set_channel(uint8_t channel) { this->channel_ = channel; }

  /// @brief The name as generated by the Python code generator
  /// @param name of the channel
  void set_channel_name(std::string name) { this->name_ = std::move(name); }

  /// @brief Get the channel name
  /// @return the name
  const char *get_channel_name() { return this->name_.c_str(); }

  /// @brief call the WK2132Register constructor
  /// @param a_register address of the register
  /// @return an WK2132Register proxy to the register at a_address
  WK2132Register channel_reg(uint8_t a_register) { return {this->parent_, a_register, this->channel_}; }

  //
  // we implement the virtual class from UARTComponent
  //

  /// @brief Writes a specified number of bytes to a serial port
  /// @param buffer pointer to the buffer
  /// @param length number of bytes to write
  /// @details This method sends 'length' characters from the buffer to the serial line. Unfortunately (unlike the
  /// Arduino equivalent) this method does not return any flag and therefore it is not possible to know if any/all bytes
  /// have been transmitted correctly. Another problem is that it is not possible to know ahead of time how many bytes
  /// we can safely send as there is no tx_available() method provided! To avoid overrun when using the write method you
  /// can use the flush() method to wait until the transmit fifo is empty.
  /// @n Typical usage could be:
  /// @code
  ///   // ...
  ///   uint8_t buffer[128];
  ///   // ...
  ///   write_array(&buffer, length);
  ///   flush();
  ///   // ...
  /// @endcode
  void write_array(const uint8_t *buffer, size_t length) override;

  /// @brief Reads a specified number of bytes from a serial port
  /// @param buffer buffer to store the bytes
  /// @param length number of bytes to read
  /// @return true if succeed, false otherwise
  /// @details Typical usage:
  /// @code
  ///   // ...
  ///   auto length = available();
  ///   uint8_t buffer[128];
  ///   if (length > 0) {
  ///     auto status = read_array(&buffer, length)
  ///     // test status ...
  ///   }
  /// @endcode
  bool read_array(uint8_t *buffer, size_t length) override;

  /// @brief Reads the first byte in FIFO without removing it
  /// @param buffer pointer to the byte
  /// @return true if succeed reading one byte, false if no character available
  /// @details This method returns the next byte from receiving buffer without removing it from the internal fifo. It
  /// returns true if a character is available and has been read, false otherwise.\n
  bool peek_byte(uint8_t *buffer) override;

  /// @brief Returns the number of bytes in the receive buffer
  /// @return the number of bytes in the receiver fifo
  int available() override;

  /// @brief Flush the output fifo.
  /// @details If we refer to Serial.flush() in Arduino it says: ** Waits for the transmission of outgoing serial data
  /// to complete. (Prior to Arduino 1.0, this the method was removing any buffered incoming serial data.). ** Therefore
  /// we wait until all bytes are gone with a timeout of 100 ms
  void flush() override;

 protected:
  friend class WK2132Component;

  /// @brief this cannot happen with external uart therefore we do nothing
  void check_logger_conflict() override {}

#ifdef TEST_COMPONENT
  /// @defgroup test_ Test component information
  /// This group contains information about the test of the component
  /// @{

  /// @brief Test the write_array() method
  /// @param message to display
  void uart_send_test_(char *message);

  /// @brief Test the read_array() method
  /// @param message to display
  /// @return true if success
  bool uart_receive_test_(char *message);
  /// @}
#endif

  /// @brief reset the wk2132 internal FIFO
  void reset_fifo_();

  /// @brief set the line parameters
  void set_line_param_();

  /// @brief set the baud rate
  void set_baudrate_();

  /// @brief Setup the channel
  void setup_channel_();

  /// @brief Returns the number of bytes in the receive fifo
  /// @return the number of bytes in the fifo
  size_t rx_in_fifo_();

  /// @brief Returns the number of bytes in the transmit fifo
  /// @return the number of bytes in the fifo
  size_t tx_in_fifo_();

  /// @brief test if transmit buffer is not empty in the status register
  /// @return true if not empty
  bool tx_fifo_is_not_empty_();

  /// @brief transfer bytes from the wk2132 internal FIFO to the buffer (if any)
  /// @return number of bytes transferred
  size_t xfer_fifo_to_buffer_();

  /// @brief the buffer where we store temporarily the bytes received
  RingBuffer<uint8_t, RING_BUFFER_SIZE> receive_buffer_;
  WK2132Component *parent_;  ///< Our WK2132component parent
  uint8_t channel_;          ///< Our Channel number
  uint8_t data_;             ///< one byte buffer for register read storage
  std::string name_;         ///< name of the entity
};

}  // namespace wk2132_i2c
}  // namespace esphome
