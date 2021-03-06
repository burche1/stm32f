// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

#include "adc.hpp"
#include "dma.hpp"
#include "dma_channel.hpp"
#include "scoped_spinlock.hpp"
#include "stm32f103.hpp"
#include "stream.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

extern "C" {
    void adc1_handler();
    void enable_interrupt( stm32f103::IRQn_type IRQn );
    void disable_interrupt( stm32f103::IRQn_type IRQn );
}

namespace stm32f103 {
    static dma_channel_t< DMA_ADC1 > * __dma_adc1;
    static uint8_t __adc1_dma[ sizeof( dma_channel_t< DMA_ADC1 > ) ];
    static std::array< uint16_t, 4 > __adc1_data;
    static std::array< uint32_t, 4 > __adc1_accumulated_data;
    static uint32_t __number_of_adc_samples;
    static constexpr uint32_t __number_of_accumulation = 4096;
};


extern stm32f103::dma __dma0;

using namespace stm32f103;

adc::adc() : adc_( 0 )
{
    // constractor may not be called if instance is declared in global space
    // init() method must be explicitly called, if this class is instanciated in .bss segment
    init( ADC1_BASE );
}

adc::~adc()
{
}

void
adc::attach( dma& dma )
{
    __dma_adc1 = new (&__adc1_dma) dma_channel_t< DMA_ADC1 >( dma, 0, 0 );
    __dma_adc1->set_receive_buffer( reinterpret_cast< uint8_t * >(__adc1_data.data()), size_t( __adc1_data.size() ) );

    adc_->CR1 |= (1 << 8); // SCAN conv mode
    adc_->CR2 |= 0x07 << 17; // SWSTART
    adc_->CR2 |= 0x01 << 1;  // Continuous conversion mode
    adc_->CR2 |= 0x01 << 8;  // DMA enable

    constexpr uint8_t sample_time = 07; // 239.5 cycles
    uint32_t smpr = 0;
    for ( size_t i = 0; i < __adc1_data.size(); ++i )
        smpr |= sample_time << (3*i);
    adc_->SMPR2 |= smpr;

    adc_->SQR1 = (3 << 20);  // p246, Regular channel sequence length [23:20] = 3 (4-1 channels to be converted)
    adc_->SQR2 = 0;          // p247, Regular channel sequence, 12th down to 7th
    adc_->SQR3 = 0|(1<<5)|(2<<10)|(3<<15);      // p248, Regular channel sequence [0->1->2->3]

    auto callback = +[]( uint32_t flag ){
        if ( flag & 02 ) { // transfer complete
            if ( ( __number_of_adc_samples++ % __number_of_accumulation ) == 0 ) {
                std::copy( __adc1_data.begin(), __adc1_data.end(), __adc1_accumulated_data.begin() );
            } else {
                std::transform( __adc1_data.begin(), __adc1_data.end()
                                , __adc1_accumulated_data.begin(), __adc1_accumulated_data.begin()
                                , [](const uint16_t& b, const uint32_t& a){ return a + b; } );
            }

            if ( ( __number_of_adc_samples % __number_of_accumulation ) == ( __number_of_accumulation - 1 ) ) {
                int i = 0;
                for ( const auto& a: __adc1_accumulated_data ) {
                    stream() << "[" << i << "]:" << int( a / __number_of_accumulation ) << "\t";
                    ++i;
                }
                stream() << std::endl;
            }
        }
    };

    __dma_adc1->set_callback( callback );

    __dma_adc1->enable( true );
}

void
adc::init( PERIPHERAL_BASE base )
{
    // RM0008, p214, ADC introduction
    // 12bit ADC, 18 multiplexed channels, max clock <= 14MHz
    // Conversion time 1.17us (@72MHz STM32F103xx)
    // 
    // RM0008, p251, ADC register map
    lock_.clear();
    flag_ = false;

    if ( auto ADC = reinterpret_cast< stm32f103::ADC * >( base ) ) {

        adc_ = ADC;

        ADC->CR1 |= (1 << 5);  // Enable end of conversion (EOC) interrupt
        ADC->SQR1 = 0;      // p246, Regular channel sequence length [23:20] = 0 (1 conversion)
        ADC->SQR2 = 0;      // p247, Regular channel sequence, 12th down to 7th
        ADC->SQR3 = 0;      // p248, Regular channel sequence, 6th to 1st (I believe this is 0 origin index, so it must be ch0)

        ADC->CR2 |= (7 << 17); // SWSTART as trigger
        ADC->CR2 |= (1 << 20); // Enable external trigger
        enable_interrupt( stm32f103::ADC1_2_IRQn );

        ADC->CR2 |= (1 << 0);  // ADON (p242)

        size_t count = 1000;
        ADC->CR2 |= (1 << 3);
        while(count-- && (ADC->CR2 & (1 << 3)))
            ; // stream() << "reset: " << ( ADC->CR2 & (1 << 3) ) << std::endl;

        ADC->CR2 |= (1 << 2);
        count = 1000;
        while(count-- && (ADC->CR2 & (1 << 2)))
            ; // stream() << "calibration: " << ( ADC->CR2 & (1 << 2)) << std::endl;
    }
}

void
adc::enable( bool onoff )
{
    if ( __dma_adc1 )
        __dma_adc1->enable( onoff );    
}

uint32_t
adc::cr2() const
{
    return adc_  ? adc_->CR2 : -1;
}

bool
adc::start_conversion()
{
    return adc_ && ( adc_->CR2 |= (1 << 22) ); // p240
}

uint16_t
adc::data()
{
    uint16_t data;
    while ( ! flag_.load() )
        ;

    scoped_spinlock<> lock( lock_ );

    data = data_;
    flag_ = false;

    return data;
}

void
adc::handle_interrupt()
{
    scoped_spinlock<> lock( lock_ );     

    data_ = adc_->DR;
    flag_ = true;
}

void
adc::interrupt_handler( adc * _this )
{
    _this->handle_interrupt();
}

adc *
adc::instance()
{
    static std::atomic_flag __once_flag;
    static adc __instance;
    if ( !__once_flag.test_and_set() )
        __instance.init( stm32f103::ADC1_BASE );
    return &__instance;
}
