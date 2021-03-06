// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

#include "bitset.hpp"
#include "dma.hpp"
#include "condition_wait.hpp"
#include "dma_channel.hpp"
#include "i2c.hpp"
#include "i2c_string.hpp"
#include "scoped_spinlock.hpp"
#include "stream.hpp"
#include "stm32f103.hpp"
#include <array>
#include <atomic>
#include <mutex>

extern uint32_t __pclk1, __pclk2;
extern void mdelay( uint32_t );

extern "C" {
    void i2c1_handler();
    void enable_interrupt( stm32f103::IRQn_type IRQn );
}

// bits in the status register
namespace stm32f103 {

    constexpr uint32_t i2c_clock_speed = 100'000; //'; //100kHz

    enum I2C_CR1_MASK {
        SWRST          = 1 << 15  // Software reset (0 := not under reset, 0 := under reset state
        , RES0         = 1 << 14  //
        , ALERT        = 1 << 13  //
        , PEC          = 1 << 12  //
        , POS          = 1 << 11  // Acknowledge/PEC Position (for data reception)
        , ACK          = 1 << 10  //
        , STOP         = 1 <<  9  //
        , START        = 1 <<  8  //
        , NOSTRETCH    = 1 <<  7  //
        , ENGC         = 1 <<  6  //
        , ENPEC        = 1 <<  5  //
        , ENARP        = 1 <<  4  //
        , SMBTYPE      = 1 <<  3  //
        , RES1         = 1 <<  2  //
        , SMBUS        = 1 <<  1  //
        , PE           = 1 <<  0  // Peripheral enable
    };

    enum I2C_CR2_MASK {
        LAST          = 1 << 12
        , DMAEN       = 1 << 11    // DMA requests enable
        , ITBUFFN     = 1 << 10    // Buffer interrupt enable
        , ITEVTEN     = 1 <<  9    // Event interrupt enable
        , ITERREN     = 1 <<  8    // Error interrupt enable
        , FREQ        = 0x3f       // Peirpheral clock frequence ( 0x02(2MHz) .. 0x32(50MHz) )
    };

   // p782
    enum I2C_STATUS {
        // SR2, p782
        ST_PEC          = 0xff << 8  // Packet error checking rigister
        , DUALF         = 1 << 7     // Dual flag (slave mode)
        , SMBHOST       = 1 << 6     // SMBus host header (slave mode)
        , SMBDEFAULT    = 1 << 5     // SMB deault, SMBus device default address (slave mode)
        , GENCALL       = 1 << 4     // General call address (slave mode)
        , TRA           = 1 << 2     // Transmitter/receiver (0: data bytes received, 1: data bytes transmitted)
        , BUSY          = 1 << 1     // Bus busy
        , MSL           = 1          // Master/slave
        // SR1, p778
        , SMB_ALART     = 1 << 15    // SMBus alert
        , TIME_OUT      = 1 << 14    // Timeout or Tlow error
        , PEC_ERR       = 1 << 12    // PEC Error in reception
        , OVR           = 1 << 11    // Overrun/Underrun
        , AF            = 1 << 10    // Acknowledge failure
        , ARLO          = 1 << 9     // arbitration lost
        , BERR          = 1 << 8     // Bus error
        , TxE           = 1 << 7     // Data register empty (transmitter)
        , RxNE          = 1 << 6     // Date register not empty (receiver)
        , STOPF         = 1 << 4     // Stop detection (slave)
        , ADD10         = 1 << 3     // 10-bit header sent
        , BTF           = 1 << 2     // Byte transfer finished
        , ADDR          = 1 << 1     // Address sent (master), matched(slave)
        , SB            = 1          // Start bit (master mode); 1: start condition generated
    };

    constexpr uint32_t error_condition = SMB_ALART | TIME_OUT | PEC_ERR | OVR | AF | ARLO | BERR;

    struct i2c_status {
        volatile I2C& _;
        i2c_status( volatile I2C& t ) : _( t ) {}
        inline bool busy() const { return _.SR2 & BUSY; }

        // p783, Reading SR2 after reading SR1 clears ADDR flag.
        inline uint32_t status() const { return ( (_.SR1 & STOPF) == 0
                                                  || ((_.SR1 & ADDR) == ADDR ) ) ? ( _.SR2 << 16 ) | _.SR1 : _.SR1; }

        inline uint32_t operator ()() const { return status(); }

        inline bool is_equal( uint32_t flags ) const {
            return ( status() & flags ) == flags;
        }

        inline bool is_equal( uint32_t flags, uint32_t mask ) const {
            return ( status() & mask ) == flags;
        }
    };

    struct i2c_reset {
        bool operator()( volatile I2C& i2c, uint8_t own_addr = 0 ) {
            if ( own_addr == 0 )
                own_addr = i2c.OAR1 >> 1;

            bitset::reset( i2c.CR1, PE );
            bitset::set( i2c.CR1, SWRST );
            while ( i2c.SR1 && i2c.SR2 )
                ;
            bitset::reset( i2c.CR1, SWRST );
            bitset::reset( i2c.CR1, PE );

            uint16_t freqrange = uint16_t( __pclk1 / 1000'000 /*'*/ ); // source clk in MHz
            uint16_t cr2 = freqrange;
            i2c.CR2 |= cr2;

            // p784,
            i2c.TRISE = freqrange + 1;
            i2c.OAR1 = own_addr << 1;
            i2c.OAR2 = 0;

            uint16_t ccr = __pclk1 / ( i2c_clock_speed * 2 );  // ratio make it 5us for 10us interval clock
            i2c.CCR = ccr;  // Sm mode

            return true;
        }
    };


    // master start
    struct i2c_start {
        volatile I2C& _;
        i2c_start( volatile I2C& t ) : _( t ) {}

        // constexpr static uint32_t master_mode_selected = ( ( BUSY | MSL ) << 16 ) | SB;

        inline bool operator()() const {
            bitset::set( _.CR1, START );
            return condition_wait()( [&]{ return _.SR1 & SB; } );
        }
    };

    struct scoped_i2c_dma_enable {
        volatile I2C& _;
        scoped_i2c_dma_enable( volatile I2C& t ) : _( t ) {
            _.CR2 |= DMAEN;
        }
        ~scoped_i2c_dma_enable() {
            _.CR2 &= ~DMAEN;
        }
    };

    struct scoped_i2c_start {
        volatile I2C& _;
        bool success;
        scoped_i2c_start( volatile I2C& t ) : _( t ), success( false ) {}

        ~scoped_i2c_start() {
            auto status = _.SR1 | ( _.SR2 << 16 ); // clear ADDR
            if ( success ) {
                bitset::set( _.CR1, STOP );
                condition_wait()( [&]{ return !bitset::test(_.SR2, BUSY); } );
            }

            bitset::reset( _.CR2, LAST );

            if ( _.SR1 & error_condition ) {
                // following condition may happens when i2cdetect attempt data read for device not on the bus
                if (( _.SR1 & AF ) && ( _.SR2 & ( BUSY | MSL ) ) ) // this error cannot recover by PE=0
                    i2c_reset()( _ );
                else
                    _.SR1 &= ~error_condition; // clear error condition
            }
        }

        bool operator()() {
            success = i2c_start( _ )();
            return success;
        }
    };

    enum I2C_DIRECTION { Transmitter, Receiver };

    template< I2C_DIRECTION >
    struct i2c_address {
        inline bool operator()( volatile I2C& _, uint8_t address ) {

            _.DR = ( address << 1 );
            return condition_wait()( [&]{ return _.SR1 & ADDR; } );
        }

        void static clear( volatile I2C& _ ) {
            auto x = i2c_status( _ )();
        }
    };

    template<> inline bool i2c_address<Receiver>::operator()( volatile I2C& _, uint8_t address ) {
        _.DR = (address << 1) | 1;
        return condition_wait()( [&](){ return _.SR1 & ADDR; } );
    }

    template<> void i2c_address<Receiver>::clear( volatile I2C& _ ) {
        auto x = i2c_status( _ )();
    }

    // AN2824, I2C optimized examples en.CD00209826.pdf
    // receiving
    template< size_t >
    struct polling_master_receiver {
        volatile I2C& _;
        polling_master_receiver( volatile I2C& t ) : _( t ) {
            bitset::set( _.CR1, PE );  // peripheral enable, ACK
        }

        I2C_RESULT_CODE operator()( uint8_t address, uint8_t * data, size_t size ) {
            scoped_i2c_start start(_);
            if ( start() ) {
                if ( i2c_address< Receiver >()( _, address ) ) {
                    i2c_address<Receiver>().clear( _ );
                    condition_wait()( [&](){ return !_.SR1 & ADDR; } );
                    while ( size >= 3 ) {
                        if ( condition_wait()( [&](){ return _.SR1 & (RxNE|BTF); } ) ) {
                            if ( _.SR1 & RxNE ) {
                                *data++ = _.DR;
                                --size;
                            }
                        } else {
                            return I2C_POLLING_MASTER_RECEIVER_RECV_TIMEOUT;
                        }
                    }
                    if ( condition_wait()( [&](){ return _.SR1 & BTF; } ) ) {
                        bitset::reset( _.CR1, ACK );
                        bitset::set( _.CR1, STOP );
                        *data++ = _.DR;  // Data N-1
                        --size;
                    } else {
                        return I2C_POLLING_MASTER_RECEIVER_RECV_TIMEOUT;
                    }
                    if ( condition_wait()( [&](){ return _.SR1 & RxNE; } ) ) {
                        *data++ = _.DR;
                        --size;
                        return I2C_RESULT_SUCCESS;
                    } else {
                        return I2C_POLLING_MASTER_RECEIVER_RECV_TIMEOUT;
                    }
                } else {
                    return I2C_POLLING_MASTER_RECEIVER_ADDRESS_FAILED;
                }
            } else {
                return I2C_POLLING_MASTER_TRANSMITTER_START_FAILED;
            }
        }
    };

    template<> I2C_RESULT_CODE polling_master_receiver< 2 >::operator()( uint8_t address, uint8_t * data, size_t size ) {
        scoped_i2c_start start(_);
        if ( start() ) {
            if ( i2c_address< Receiver >()( _, address ) ) {
                bitset::set( _.CR1, POS );
                i2c_address<Receiver>().clear( _ );
                bitset::reset( _.CR1, ACK );
                if ( condition_wait()( [&](){ return _.SR1 & BTF; } ) ) {
                    bitset::set( _.CR1, STOP );
                    *data++ = _.DR;
                    --size;
                    if ( condition_wait()( [&](){ return _.SR1 & (RxNE|BTF); } ) ) {
                        *data++ = _.DR;
                        --size;
                        if ( condition_wait()( [&](){ return !bitset::test(_.SR2, BUSY); } ) ) {
                            bitset::reset( _.CR1, POS );
                        }
                    }
                }
                return size == 0 ? I2C_RESULT_SUCCESS : I2C_POLLING_MASTER_RECEIVER_RECV_TIMEOUT;
            }
        }
        return I2C_POLLING_MASTER_TRANSMITTER_START_FAILED;
    }

    template<> I2C_RESULT_CODE polling_master_receiver< 1 >::operator()( uint8_t address, uint8_t * data, size_t size ) {
        scoped_i2c_start start(_);
        if ( start() ) {
            if ( i2c_address< Receiver >()( _, address ) ) {
                bitset::reset( _.CR1, ACK );           // ACK = 0
                i2c_address<Receiver>().clear( _ );    // Clear ADDR
                bitset::set( _.CR1, STOP );            // STOP = 1
                if ( condition_wait()( [&](){ return _.SR1 & RxNE; } ) ) {  // Wait until RxNE = 1
                    *data++ = _.DR;                    // Read the data
                    --size;
                }
                return size == 0 ? I2C_RESULT_SUCCESS : I2C_POLLING_MASTER_RECEIVER_RECV_TIMEOUT;
            } else
                return I2C_POLLING_MASTER_RECEIVER_ADDRESS_FAILED;
        } else
            return I2C_POLLING_MASTER_RECEIVER_START_FAILED;
    }
    /////////////////////////////////////////////////////

    // master transmit
    struct i2c_transmitter {
        volatile I2C& _;
        i2c_transmitter( volatile I2C& t ) : _( t ) {}

        inline bool operator << ( uint8_t data ) {
            _.DR = data;
            return condition_wait()( [&](){ return _.SR1 & TxE|BTF; } );
        }
    };

    struct i2c_ready_wait {
        volatile I2C& _;
        uint8_t own_addr_;

        i2c_ready_wait( volatile I2C& t, uint8_t own_addr ) : _( t ), own_addr_( own_addr ) {}

        I2C_RESULT_CODE operator()( bool reset = false ) const {
            i2c_status st( _ );

            if ( _.SR1 & error_condition ) {
                _.SR1 &= ~error_condition;
                if ( _.SR1 & error_condition )
                    return I2C_DEVICE_ERROR_CONDITION;
            }

            if ( ! condition_wait()( [&](){ return !st.busy(); } ) ) {
                if ( ( _.SR1 & error_condition ) && ( _.SR2 & (BUSY| MSL) ) )
                    i2c_reset()( _, own_addr_ );  // Reset i2c chip
            }

            return st.busy() ? I2C_BUS_BUSY : I2C_RESULT_SUCCESS;
        }
    };

    // STM32F1xx I2C seems has issue such as
    // Data transfer by dma is alway size - 1 except for size == 1
    // If remove ADDR clear statement, then either always send 1 byte or transmit fail.

    struct dma_master_transfer {
        volatile I2C& _;
        dma_master_transfer( volatile I2C& t ) : _( t ) {
            bitset::set( _.CR1, ACK | PE );  // peripheral enable, ACK
        }

        template< typename T >
        I2C_RESULT_CODE operator()( T& dma_channel, uint8_t address, const uint8_t * data, size_t size ) const {

            scoped_i2c_start start( _ );

            dma_channel.set_transfer_buffer( data, size == 1 ? 1 : size + 1 ); // workaround
            scoped_dma_channel_enable enable_dma_channel( dma_channel );
            scoped_i2c_dma_enable dma_enable( _ );

            if ( start() ) { // generate start condition (master start)

                if ( i2c_address< Transmitter >()( _, address ) ) {
                    i2c_address< Transmitter >().clear( _ );

                    if ( condition_wait()( [&]{ return dma_channel.transfer_complete(); } ) )
                        return I2C_RESULT_SUCCESS;
                    else
                        return I2C_DMA_MASTER_TRANSMITTER_SEND_TIMEOUT;

                } else {
                    return I2C_DMA_MASTER_TRANSMITTER_ADDRESS_FAILED;
                }
            } else
                return I2C_DMA_MASTER_TRANSMITTER_ADDRESS_FAILED;
        }
    };

    struct dma_master_receiver {
        volatile I2C& _;
        dma_master_receiver( volatile I2C& t ) : _( t ) {
            bitset::set( _.CR1, PE );  // peripheral enable
        }

        template< typename T >
        I2C_RESULT_CODE operator()( T& dma_channel, uint8_t address, uint8_t * data, size_t size ) const {

            dma_channel.set_receive_buffer( data, size );
            scoped_dma_channel_enable dma_channel_enable( dma_channel );
            scoped_i2c_dma_enable dma_enable( _ ); // DMAEN set

            bitset::set( _.CR2, LAST );
            scoped_i2c_start start( _ );
            if ( start() ) { // generate start condition (master start)
                if ( i2c_address< Receiver >()( _, address ) ) {
                    i2c_address< Receiver >::clear(_);
                    if ( condition_wait()( [&](){ return dma_channel.transfer_complete(); } ) ) {
                        return I2C_RESULT_SUCCESS;
                    } else
                        return I2C_DMA_MASTER_RECEIVER_RECV_TIMEOUT;
                } else
                    return I2C_DMA_MASTER_RECEIVER_ADDRESS_FAILED;
            } else
                return I2C_DMA_MASTER_RECEIVER_START_FAILED;
        }

    };

}

using namespace stm32f103;

static uint8_t __i2c1_tx_dma[ sizeof( dma_channel_t< DMA_I2C1_TX > ) ];
static uint8_t __i2c1_rx_dma[ sizeof( dma_channel_t< DMA_I2C1_RX > ) ];
static uint8_t __i2c2_tx_dma[ sizeof( dma_channel_t< DMA_I2C2_TX > ) ];
static uint8_t __i2c2_rx_dma[ sizeof( dma_channel_t< DMA_I2C2_RX > ) ];

static dma_channel_t< DMA_I2C1_TX > * __dma_i2c1_tx;
static dma_channel_t< DMA_I2C1_RX > * __dma_i2c1_rx;
static dma_channel_t< DMA_I2C2_TX > * __dma_i2c2_tx;
static dma_channel_t< DMA_I2C2_RX > * __dma_i2c2_rx;

i2c::i2c() : i2c_( 0 )
{
}

void
i2c::attach( dma& dma, DMA_Direction dir )
{
    uint32_t addr = reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_));

    if ( addr == I2C1_BASE ) {
        if ( dir == DMA_Rx || dir == DMA_Both ) {
            if ( __dma_i2c1_rx = new (&__i2c1_rx_dma) dma_channel_t< DMA_I2C1_RX >( dma, 0, 0 ) ) {
                __dma_i2c1_rx->set_callback( +[]( uint32_t flag ){
                        // stream() << "\n\tI2C-1 Rx irq: " << flag << std::endl;
                    });
            }
        }
        if ( dir == DMA_Tx || dir == DMA_Both ) {
            if ( __dma_i2c1_tx = new (&__i2c1_tx_dma) dma_channel_t< DMA_I2C1_TX >( dma, 0, 0 ) ) {
                __dma_i2c1_tx->set_callback( +[]( uint32_t flag ){
                        // stream() << "\n\tI2C-1 Tx irq: " << flag << std::endl;
                    });
            }
        }
    } else if ( addr == I2C2_BASE ) {
        if ( dir == DMA_Rx || dir == DMA_Both ) {
            if ( __dma_i2c2_rx = new (&__i2c2_rx_dma) dma_channel_t< DMA_I2C2_RX >( dma, 0, 0 ) ) {
                __dma_i2c2_rx->set_callback( +[]( uint32_t flag ){
                        // stream() << "\n\tI2C-2 Rx irq: " << flag << std::endl;
                    });
            }
        }
        if ( dir == DMA_Tx || dir == DMA_Both ) {
            if ( __dma_i2c2_tx = new (&__i2c2_tx_dma) dma_channel_t< DMA_I2C2_TX >( dma, 0, 0 ) ) {
                __dma_i2c2_tx->set_callback( +[]( uint32_t flag ){
                        // stream() << "\n\tI2C-2 Tx irq: " << flag << std::endl;
                    });
            }
        }
    }
}

void
i2c::init( stm32f103::I2C_BASE addr )
{
    lock_.clear();
    own_addr_ = ( addr == I2C1_BASE ) ? 0x03 : 0x04;

    if ( auto I2C = reinterpret_cast< volatile stm32f103::I2C * >( addr ) ) {
        i2c_ = I2C;

        reset();

        switch ( addr ) {
        case I2C1_BASE:
            enable_interrupt( I2C1_EV_IRQn );
            enable_interrupt( I2C1_ER_IRQn );
            break;
        case I2C2_BASE:
            enable_interrupt( I2C2_EV_IRQn );
            enable_interrupt( I2C2_ER_IRQn );
            break;
        }
    }

}

bool
i2c::listen( uint8_t addr )
{
    own_addr_ = addr;

    i2c_->OAR1 = own_addr_ << 1;
    bitset::set( i2c_->CR1, ACK );
    bitset::set( i2c_->CR2, ITEVTEN | ITERREN );

    return true;
}

void
i2c::reset()
{
    i2c_reset()( *i2c_, own_addr_ );
}

bool
i2c::has_dma( DMA_Direction dir ) const
{
    if ( reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_)) == I2C1_BASE ) {
        if ( dir == DMA_Rx )
            return __dma_i2c1_rx != nullptr;
        else if ( dir == DMA_Tx )
            return __dma_i2c1_tx != nullptr;
        else if ( dir == DMA_Both )
            return __dma_i2c1_rx != nullptr && __dma_i2c1_tx != nullptr;
        else
            return __dma_i2c1_rx == nullptr && __dma_i2c1_tx == nullptr;
    } else if ( reinterpret_cast< uint32_t >(const_cast< I2C * >(i2c_)) == I2C2_BASE ) {
        if ( dir == DMA_Rx )
            return __dma_i2c2_rx != nullptr;
        else if ( dir == DMA_Tx )
            return __dma_i2c2_tx != nullptr;
        else if ( dir == DMA_Both )
            return __dma_i2c2_rx != nullptr && __dma_i2c2_tx != nullptr;
        else
            return __dma_i2c2_rx == nullptr && __dma_i2c2_tx == nullptr;
    }
    return false;
}

I2C_RESULT_CODE
i2c::result_code() const
{
    return result_code_;
}

stream&
i2c::print_result( stream&& o ) const
{
    auto code = result_code();
    switch( code ) {
    case I2C_RESULT_SUCCESS:
        o << "success"; break;
    case I2C_BUS_BUSY:
        o << "i2c bus busy"; break;
    case I2C_DMA_MASTER_RECEIVER_HAS_NO_DMA:
        o << "i2c dma master receiver has no dma"; break;
    case I2C_DMA_MASTER_RECEIVER_START_FAILED:
        o << "i2c dma master receiver start failed"; break;
    case I2C_DMA_MASTER_RECEIVER_ADDRESS_FAILED:
        o << "i2c dma master receiver address failed"; break;
    case I2C_DMA_MASTER_RECEIVER_RECV_TIMEOUT:
        o << "i2c dma master receiver recv timeout"; break;
    case I2C_DMA_MASTER_TRANSMITTER_HAS_NO_DMA:
        o << "i2c dma master transmitter has no dma"; break;
    case I2C_DMA_MASTER_TRANSMITTER_START_FAILED:
        o << "i2c dma master transmitter start failed"; break;
    case I2C_DMA_MASTER_TRANSMITTER_ADDRESS_FAILED:
        o << "i2c dma master transmitter address failed"; break;
    case I2C_DMA_MASTER_TRANSMITTER_SEND_TIMEOUT:
        o << "i2c dma master transmitter send timeout"; break;
    default:
        o << "error code: " << code << "\t";
        break;
    }
    return o;
}

uint32_t
i2c::status() const
{
    return i2c_status( *i2c_ )();
}

stream&
i2c::print_status( stream&& o ) const
{
    i2c_string::print_registers( std::move( o ), i2c_ );

    int count = 10;
    while( --count && ( i2c_->SR2 & BUSY ) )
        mdelay( 1 );

    if ( count == 0 )
        o << "I2C keep busy -- check SDA line, it must be high" << std::endl;

    return o;
}


bool
i2c::read( uint8_t address, uint8_t * data, size_t size )
{
    scoped_spinlock<> lock( lock_ );

    if ( ( result_code_ = i2c_ready_wait( *i2c_, own_addr_ )() ) != I2C_RESULT_SUCCESS )
        return false;

    if ( size > 3 )
        result_code_ = polling_master_receiver<3>( *i2c_ )( address, data, size );
    if ( size == 2 )
        result_code_ = polling_master_receiver<2>( *i2c_ )( address, data, size );
    if ( size == 1 )
        result_code_ = polling_master_receiver<1>( *i2c_ )( address, data, size );
    return result_code_ == I2C_RESULT_SUCCESS;
}

bool
i2c::write( uint8_t address, const uint8_t * data, size_t size )
{
    scoped_spinlock<> lock( lock_ );

    bitset::set( i2c_->CR1, ACK | PE );

    if ( ( result_code_ = i2c_ready_wait( *i2c_, own_addr_ )() ) != I2C_RESULT_SUCCESS )
        return false;

    scoped_i2c_start start( *i2c_ );

    if ( start() ) {
        if ( i2c_address< Transmitter >()( *i2c_, address ) ) { // address phase
            i2c_transmitter writer( *i2c_ );
            while ( size ) {
                if ( ( writer << *data++ ) )
                    --size;
                else {
                    break;
                }
            }
            bitset::set( i2c_->CR1, STOP );
            if ( size == 0 )
                result_code_ = I2C_RESULT_SUCCESS;
            else
                result_code_ = I2C_POLLING_MASTER_TRANSMITTER_SEND_TIMEOUT;
        } else {
            result_code_ = I2C_POLLING_MASTER_TRANSMITTER_ADDRESS_FAILED;
        }
    } else {
        result_code_ = I2C_POLLING_MASTER_TRANSMITTER_START_FAILED;
    }
    return result_code_ == I2C_RESULT_SUCCESS;
}

bool
i2c::dma_transfer( uint8_t address, const uint8_t * data, size_t size )
{
    scoped_spinlock<> lock( lock_ );

    const auto base_addr = reinterpret_cast< uint32_t >( const_cast< I2C * >(i2c_) );
    if ( base_addr == I2C1_BASE && __dma_i2c1_tx == nullptr ) {
        result_code_ = I2C_DMA_MASTER_TRANSMITTER_HAS_NO_DMA;
        return false;
    } else if ( base_addr == I2C2_BASE && __dma_i2c2_tx == nullptr ) {
        result_code_ = I2C_DMA_MASTER_TRANSMITTER_HAS_NO_DMA;
        return false;
    }

    if ( ( result_code_ = i2c_ready_wait( *i2c_, own_addr_ )() ) != I2C_RESULT_SUCCESS )
        return false;


    if ( base_addr == I2C1_BASE ) {

        result_code_ = dma_master_transfer( *i2c_ )( *__dma_i2c1_tx, address, data, size );
        return result_code_ == I2C_RESULT_SUCCESS;

    } else if ( base_addr == I2C2_BASE ) { // && __dma_i2c2_tx != nullptr ) {

        result_code_ = dma_master_transfer( *i2c_ )( *__dma_i2c2_tx, address, data, size );
        return result_code_ == I2C_RESULT_SUCCESS;

    }

    return false;
}

bool
i2c::dma_receive( uint8_t address, uint8_t * data, size_t size )
{
    scoped_spinlock<> lock( lock_ );

    const auto base_addr = reinterpret_cast< uint32_t >( const_cast< I2C * >(i2c_) );

    if ( base_addr == I2C1_BASE && __dma_i2c1_rx == nullptr ) {
        result_code_ = I2C_DMA_MASTER_RECEIVER_HAS_NO_DMA;
        return false;
    } else if ( base_addr == I2C2_BASE && __dma_i2c2_rx == nullptr ) {
        result_code_ = I2C_DMA_MASTER_RECEIVER_HAS_NO_DMA;
        return false;
    }

    if ( ( result_code_ = i2c_ready_wait( *i2c_, own_addr_ )() ) != I2C_RESULT_SUCCESS )
        return false;

    if ( size == 1 ) {
        // See AN2824, p10, "master reception of a single byte is not supported."
        return polling_master_receiver< 1 >( *i2c_ )( address, data, size );
    }

    if ( base_addr == I2C1_BASE ) {
        result_code_ = dma_master_receiver( *i2c_ )( *__dma_i2c1_rx, address, data, size );
        return result_code_ == I2C_RESULT_SUCCESS;
    } else if ( base_addr == I2C2_BASE ) {
        result_code_ = dma_master_receiver( *i2c_ )( *__dma_i2c2_rx, address, data, size );
        return result_code_ == I2C_RESULT_SUCCESS;
    }

    return false;
}

void
i2c::handle_event_interrupt()
{
}

void
i2c::handle_error_interrupt()
{
    // stream() << "ERROR irq: " << status32_to_string( i2c_status( *i2c_ )() ) << std::endl;
    constexpr uint32_t error_condition = SMB_ALART | TIME_OUT | PEC_ERR | OVR | AF | ARLO | BERR;
    i2c_->SR1 &= ~error_condition;
}
