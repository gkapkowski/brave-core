/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this file,
* You can obtain one at http://mozilla.org/MPL/2.0/. */

import * as React from 'react'
import createWidget from '../widget/index'
import { getLocale } from '../../../../common/locale'

import {
  WidgetWrapper,
  Header,
  Content,
  WelcomeText,
  InputLabel,
  NameInputWrapper,
  NameInputField,
  ActionsWrapper,
  CallButton,
  TogetherIcon,
  StyledTitle,
  StyledTitleText
} from './style'
import { StyledTitleTab } from '../widgetTitleTab'
import BraveTogetherIcon from './assets/brave-together-icon'

export interface TogetherProps {
  showContent: boolean
  onShowContent: () => void
  onCreateCall: (name: string) => void
}

class Together extends React.PureComponent<TogetherProps, {}> {

  getButtonText = () => {
    return getLocale('togetherWidgetStartButton')
  }

  renderTitle () {
    return (
      <Header>
        <StyledTitle>
          <TogetherIcon>
            <BraveTogetherIcon />
          </TogetherIcon>
          <StyledTitleText>
          {getLocale('togetherWidgetTitle')}
          </StyledTitleText>
        </StyledTitle>
      </Header>
    )
  }

  renderTitleTab () {
    const { onShowContent } = this.props

    return (
      <StyledTitleTab onClick={onShowContent}>
        {this.renderTitle()}
      </StyledTitleTab>
    )
  }

  shouldCreateCall = () => {
    this.props.onCreateCall('coolroom')
  }

  render () {
    const {
      showContent
    } = this.props

    if (!showContent) {
      return this.renderTitleTab()
    }

    return (
      <WidgetWrapper>
          {this.renderTitle()}
          <Content>
            <WelcomeText>
              {getLocale('togetherWidgetWelcomeTitle')}
            </WelcomeText>
            <InputLabel>
              {getLocale('togetherWidgetRoomNameLabel')}
            </InputLabel>
            <NameInputWrapper>
              <NameInputField placeholder={'coolroom'}></NameInputField>
            </NameInputWrapper>
            <ActionsWrapper>
              <CallButton onClick={this.shouldCreateCall}>
                {getLocale('togetherWidgetStartButton')}
              </CallButton>
            </ActionsWrapper>
          </Content>
      </WidgetWrapper>
    )
  }
}

export const TogetherWidget = createWidget(Together)
